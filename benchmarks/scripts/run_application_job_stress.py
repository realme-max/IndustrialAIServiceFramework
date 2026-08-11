#!/usr/bin/env python3
"""Measure bounded Application Job queue backpressure with a synthetic adapter.

This is a framework queue benchmark, not PTV2, WeldAgent, GPU, or AI
inference performance.  It drives the production HTTP and application paths
with a local fixture executable and records only bounded aggregate evidence.
"""

from __future__ import annotations

import argparse
import http.client
import json
import math
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Sequence

try:
    from benchmark_common import (
        BaselineError,
        SCHEMA_VERSION,
        atomic_write_bytes,
        atomic_write_csv,
        atomic_write_json,
        create_unique_run_directory,
        environment_snapshot,
        git_snapshot,
        make_run_id,
        port_is_available,
        probe_http,
        sha256_file,
        utc_now,
        validate_loopback_host,
        validate_port,
    )
    from run_baseline_smoke import _PipeCapture, _process_group_reaped, _terminate_process
    from run_http_benchmark import _ProcessSampler, _read_process_usage
except ImportError:  # pragma: no cover
    from .benchmark_common import (
        BaselineError,
        SCHEMA_VERSION,
        atomic_write_bytes,
        atomic_write_csv,
        atomic_write_json,
        create_unique_run_directory,
        environment_snapshot,
        git_snapshot,
        make_run_id,
        port_is_available,
        probe_http,
        sha256_file,
        utc_now,
        validate_loopback_host,
        validate_port,
    )
    from .run_baseline_smoke import _PipeCapture, _process_group_reaped, _terminate_process
    from .run_http_benchmark import _ProcessSampler, _read_process_usage


SCENARIO = "application-job-stress"
PROFILE_ATTEMPTS = (100, 250, 500)
QUEUE_CAPACITY = 128
REPOSITORY_CAPACITY = 1024
SUBMIT_WORKERS = 64
START_TIMEOUT_SECONDS = 30.0
DRAIN_TIMEOUT_SECONDS = 180.0
REQUEST_TIMEOUT_SECONDS = 5.0
SAMPLER_INTERVAL_SECONDS = 0.1
JOB_ID = re.compile(r"^wi_[0-9a-f]{32}$")
INPUT_ARTIFACT_ID = re.compile(r"^pc_[0-9a-f]{64}$")
KNOWN_STATES = {
    "accepted", "queued", "dispatching", "running", "cancelling",
    "succeeded", "waiting_human", "failed", "cancelled", "timed_out", "worker_lost",
}
CSV_FIELDS = (
    "scenario", "job_attempts", "submit_workers", "queue_capacity", "repository_capacity",
    "accepted", "queue_full", "unexpected_failures", "blocker_submit_latency_ms",
    "measured_submit_requests", "batch_submission_duration_seconds",
    "batch_submission_requests_per_second", "all_latency_p50_ms", "all_latency_p95_ms",
    "all_latency_p99_ms", "all_latency_max_ms", "accepted_latency_p50_ms",
    "accepted_latency_p95_ms", "accepted_latency_p99_ms", "queue_full_latency_p50_ms",
    "queue_full_latency_p95_ms", "queue_full_latency_p99_ms", "running_before_release",
    "queued_before_release", "drain_duration_seconds", "drain_jobs_per_second",
    "succeeded", "recovery_accepted", "recovery_succeeded", "post_load_health_ok",
    "server_cpu_average_percent", "server_cpu_peak_percent", "server_rss_initial_bytes",
    "server_rss_peak_bytes", "resource_sample_count", "client_cpu_seconds",
    "drain_completed_jobs", "drain_remaining_jobs", "drain_last_progress_elapsed_seconds",
    "drain_stalled_seconds", "drain_state_queued", "drain_state_dispatching",
    "drain_state_running", "drain_state_succeeded", "drain_state_cancelling",
    "drain_state_waiting_human", "drain_state_failed", "drain_state_cancelled",
    "drain_state_timed_out", "drain_state_worker_lost", "drain_state_other",
    "drain_health_ok", "drain_server_alive", "drain_blocker_marker",
    "drain_started_marker", "drain_release_marker", "drain_sampler_ok",
    "drain_diagnostic_complete", "drain_diagnostic_error", "drain_timeout_attempts_profile",
    "drain_fixture_started", "drain_fixture_finished", "drain_fixture_inflight",
    "drain_output_result_files", "drain_output_prediction_files", "drain_scratch_files",
    "drain_output_file_count", "drain_manifest_count", "drain_stall_snapshot_count",
    "drain_final_snapshot_count", "drain_stall_snapshot_capability",
    "drain_final_snapshot_capability", "drain_stall_snapshot_error",
    "drain_final_snapshot_error", "drain_gdb_status", "drain_stall_snapshot_json",
    "drain_final_snapshot_json",
)

DRAIN_STATE_NAMES = (
    "queued", "dispatching", "running", "succeeded", "cancelling",
    "waiting_human", "failed", "cancelled", "timed_out", "worker_lost",
)


def _method_metadata() -> dict[str, Any]:
    return {
        "scope": "synthetic adapter / framework queue stress",
        "not_ptv2_inference_performance": True,
        "not_weldagent_performance": True,
        "not_gpu_benchmark": True,
        "not_ai_performance": True,
        "not_queue_only_throughput": True,
        "not_application_acceptance_capacity": True,
        "load_generator": "Python standard-library http.client",
        "client_server_same_host": True,
        "cpu_percent_definition": "100% = one logical CPU core",
        "rss_source_unit": "bytes",
        "rss_display_unit": "MiB",
        "application_worker": "single production worker",
        "submit_workers": SUBMIT_WORKERS,
        "submission_throughput_scope": "accepted and queue_full HTTP responses",
        "drain_scope": (
            "application single-worker drain including LocalProcessRunner fork/exec, "
            "synthetic fixture execution, adapter file handling, repository transitions, "
            "and HTTP status polling"
        ),
        "resource_scope": "IAISF server PID only; synthetic child processes excluded",
        "resource_measurement_excludes": "blocker setup before formal batch submission",
        "stall_snapshot_trigger": "one snapshot after five seconds without completed-job progress",
        "proc_snapshot_scope": "bounded aggregate Linux /proc thread and direct-child state only",
    }


def _finite(value: float | int | None) -> float | int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not math.isfinite(float(value)):
        raise BaselineError("invalid_measurement", "measurement is not finite")
    return value


def _percentile(values: Sequence[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return round(ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower), 3)


def _validate_profiles(attempts: Sequence[int]) -> tuple[int, ...]:
    result = tuple(attempts)
    if result != PROFILE_ATTEMPTS:
        raise BaselineError("invalid_profiles", "job attempts must be 100, 250, and 500")
    if QUEUE_CAPACITY != 128 or REPOSITORY_CAPACITY != 1024 or SUBMIT_WORKERS != 64:
        raise BaselineError("invalid_parameters", "stress capacity constants are invalid")
    return result


def _validate_fixture_executable(fixture: Path) -> None:
    if (
        not fixture.is_file()
        or fixture.is_symlink()
        or (os.name == "posix" and not os.access(fixture, os.X_OK))
    ):
        raise BaselineError("fixture_not_executable", "benchmark fixture is not executable")


def _batch_submission_metrics(
    measured_requests: int, started: float, finished: float
) -> tuple[int, float, float]:
    duration = finished - started
    if measured_requests <= 0 or not math.isfinite(duration) or duration <= 0.0:
        raise BaselineError("invalid_measurement", "batch submission interval is invalid")
    return measured_requests, duration, measured_requests / duration


def _copy_fixture_to_work_root(
    fixture: Path, work_root: Path, destination: Path | None = None
) -> Path:
    if fixture.is_symlink() or not fixture.is_file():
        raise BaselineError("fixture_source_invalid", "benchmark fixture source is invalid")
    _validate_fixture_executable(fixture)
    try:
        canonical_root = work_root.resolve(strict=True)
        bin_dir = canonical_root / "bin"
        if bin_dir.exists() or bin_dir.is_symlink():
            if not bin_dir.is_dir() or bin_dir.is_symlink():
                raise BaselineError("fixture_copy_failure", "benchmark fixture target is invalid")
        else:
            bin_dir.mkdir()
        target = destination or (bin_dir / fixture.name)
        target_parent = target.parent.resolve(strict=True)
        if target_parent != bin_dir.resolve(strict=True):
            raise BaselineError("fixture_copy_failure", "benchmark fixture target escapes work root")
        if target.exists() or target.is_symlink():
            raise BaselineError("fixture_copy_failure", "benchmark fixture target is occupied")
        source_sha = sha256_file(fixture)
        shutil.copyfile(fixture, target)
        os.chmod(target, stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR)
        if (
            target.is_symlink()
            or not target.is_file()
            or target.resolve(strict=True).parent != bin_dir.resolve(strict=True)
            or not os.access(target, os.X_OK)
            or sha256_file(target) != source_sha
        ):
            raise BaselineError("fixture_copy_failure", "benchmark fixture copy validation failed")
        return target
    except BaselineError:
        if "target" in locals():
            try:
                if target.is_file() and not target.is_symlink():
                    target.unlink()
            except OSError:
                pass
        raise
    except (OSError, ValueError) as exc:
        if "target" in locals():
            try:
                if target.is_file() and not target.is_symlink():
                    target.unlink()
            except OSError:
                pass
        raise BaselineError("fixture_copy_failure", "benchmark fixture copy failed") from exc


def _state_counts(states: Sequence[str]) -> dict[str, int]:
    counts = {state: 0 for state in DRAIN_STATE_NAMES}
    counts["other"] = 0
    for state in states:
        if state in counts:
            counts[state] += 1
        else:
            counts["other"] += 1
    return counts


def _bounded_file_counts(root: Path, *, names: set[str] | None = None) -> int:
    """Count only bounded aggregate file evidence; never expose a path."""
    if not root.is_dir() or root.is_symlink():
        return 0
    count = 0
    try:
        for directory, _subdirectories, files in os.walk(root):
            for name in files:
                if names is None or name in names:
                    count += 1
                    if count >= 10000:
                        return 10000
    except OSError:
        return 0
    return count


def _workspace_aggregate(work_root: Path) -> dict[str, int]:
    markers = work_root / "markers"
    started = _bounded_file_counts(markers / "invocation_started")
    finished = _bounded_file_counts(markers / "invocation_finished")
    return {
        "fixture_started_count": started,
        "fixture_finished_count": finished,
        "fixture_inflight_count": max(0, started - finished),
        "output_result_file_count": _bounded_file_counts(work_root / "outputs", names={"weld_result.json"}),
        "output_prediction_file_count": _bounded_file_counts(work_root / "outputs", names={"prediction.txt"}),
        "scratch_file_count": _bounded_file_counts(work_root / "scratch"),
        "output_file_count": _bounded_file_counts(work_root / "outputs"),
        "manifest_count": _bounded_file_counts(work_root / "artifacts", names={"artifact.json", "manifest.json"}),
    }


_SYSCALL_NAMES = {
    0: "read", 1: "write", 7: "poll", 35: "nanosleep", 61: "wait4",
    72: "fcntl", 202: "futex", 232: "epoll_wait", 271: "ppoll",
}


def _safe_proc_text(path: Path, limit: int = 128) -> str | None:
    try:
        value = path.read_text(encoding="ascii", errors="replace").strip()
    except (OSError, UnicodeError):
        return None
    value = value[:limit]
    if not re.fullmatch(r"[A-Za-z0-9_.:+? -]{0,128}", value):
        return "unknown"
    return value or "unknown"


def _proc_thread_snapshot(server_pid: int) -> dict[str, Any]:
    """Return bounded, non-sensitive aggregate /proc evidence."""
    if os.name != "posix":
        return {"available": False, "error": "capability_unavailable"}
    task_root = Path("/proc") / str(server_pid) / "task"
    try:
        tids = sorted(int(item.name) for item in task_root.iterdir() if item.name.isdigit())
    except (OSError, ValueError):
        return {"available": False, "error": "capability_unavailable"}
    state_counts: dict[str, int] = {}
    wchan_counts: dict[str, int] = {}
    syscall_counts: dict[str, int] = {}
    child_ids: set[int] = set()
    for tid in tids[:1024]:
        base = task_root / str(tid)
        state = "unknown"
        try:
            status = (base / "status").read_text(encoding="ascii", errors="replace")[:4096]
            match = re.search(r"^State:\s+([A-Z])", status, re.MULTILINE)
            state = match.group(1) if match else "unknown"
        except (OSError, UnicodeError):
            pass
        wchan = _safe_proc_text(base / "wchan") or "unknown"
        syscall_text = _safe_proc_text(base / "syscall") or "unknown"
        syscall_key = syscall_text.split(" ", 1)[0]
        try:
            syscall_key = _SYSCALL_NAMES.get(int(syscall_key), f"syscall_{int(syscall_key)}")
        except ValueError:
            syscall_key = "unknown"
        state_counts[state] = state_counts.get(state, 0) + 1
        wchan_counts[wchan] = wchan_counts.get(wchan, 0) + 1
        syscall_counts[syscall_key] = syscall_counts.get(syscall_key, 0) + 1
        try:
            children = (base / "children").read_text(encoding="ascii", errors="replace")
            child_ids.update(int(token) for token in children.split() if token.isdigit())
        except (OSError, UnicodeError, ValueError):
            pass
    child_states: dict[str, int] = {}
    child_wchans: dict[str, int] = {}
    child_zombies = 0
    child_uninterruptible = 0
    existing_children = 0
    for child in sorted(child_ids)[:1024]:
        child_base = Path("/proc") / str(child)
        if not child_base.exists():
            continue
        existing_children += 1
        state = "unknown"
        try:
            stat_line = (child_base / "stat").read_text(encoding="ascii", errors="replace")
            state_part = stat_line.rsplit(")", 1)[-1].strip().split(" ", 1)
            state = state_part[0] if state_part else "unknown"
        except (OSError, UnicodeError):
            pass
        child_states[state] = child_states.get(state, 0) + 1
        child_wchan = _safe_proc_text(child_base / "wchan") or "unknown"
        child_wchans[child_wchan] = child_wchans.get(child_wchan, 0) + 1
        child_zombies += int(state == "Z")
        child_uninterruptible += int(state == "D")
    interesting = {"wait4": False, "waitpid": False, "poll": False, "ppoll": False,
                   "read": False, "futex": False, "filesystem_io": False}
    for key in list(wchan_counts) + list(syscall_counts):
        lowered = key.lower()
        for name in ("wait4", "waitpid", "poll", "ppoll", "read", "futex"):
            if name in lowered:
                interesting[name] = True
        if any(token in lowered for token in ("io_schedule", "fsync", "writeback", "file", "open")):
            interesting["filesystem_io"] = True
    return {
        "available": True, "thread_count": len(tids), "state_counts": state_counts,
        "wchan_counts": wchan_counts, "syscall_counts": syscall_counts,
        "interesting": interesting, "direct_child_count": existing_children,
        "direct_child_state_counts": child_states, "direct_child_wchan_counts": child_wchans,
        "direct_child_zombie_count": child_zombies,
        "direct_child_uninterruptible_count": child_uninterruptible,
    }


def _capture_gdb_snapshot(server_pid: int, run_dir: Path, attempts: int) -> str:
    if os.name != "posix" or shutil.which("gdb") is None:
        return "capability_unavailable"
    diagnostic_dir = run_dir / "diagnostics"
    try:
        diagnostic_dir.mkdir(exist_ok=True)
        output_path = diagnostic_dir / f"stall-gdb-{attempts}.txt"
        process = subprocess.Popen(
            ["gdb", "-batch", "-nx", "-ex", "set pagination off",
             "-ex", "thread apply all bt 8", "-ex", "detach", "-ex", "quit", "-p", str(server_pid)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace",
        )
        try:
            output, _ = process.communicate(timeout=5.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            return "timeout"
        output_path.write_text(output[:256 * 1024], encoding="utf-8")
        return "captured" if process.returncode == 0 else "failed"
    except (OSError, ValueError):
        return "failed"


def _capture_stall_snapshot(
    row: dict[str, Any], *, run_dir: Path, host: str, port: int, attempts: int,
    drain_started: float, completed: int, remaining: int, states: Sequence[str],
    last_progress_at: float, process: subprocess.Popen[bytes] | None,
    sampler: _ProcessSampler | None, work_root: Path, kind: str,
) -> None:
    """Capture one bounded aggregate snapshot; failure never replaces drain_timeout."""
    key = "stall" if kind == "stall" else "final"
    try:
        now = time.monotonic()
        workspace = _workspace_aggregate(work_root)
        proc = _proc_thread_snapshot(process.pid) if process is not None else {
            "available": False, "error": "server_unavailable"
        }
        health_ok = False
        try:
            health_ok = bool(probe_http(host, port, "/health", REQUEST_TIMEOUT_SECONDS).get("ok"))
        except Exception:
            health_ok = False
        aggregate = {
            "completed": completed, "pending": remaining, "state_counts": _state_counts(states),
            "last_progress_elapsed_seconds": round(max(0.0, last_progress_at - drain_started), 3),
            "no_progress_seconds": round(max(0.0, now - last_progress_at), 3),
            **workspace, "server_alive": bool(process is not None and process.poll() is None),
            "health_ok": health_ok, "blocker_marker": (work_root / "markers" / "blocker_claimed").is_file(),
            "started_marker": (work_root / "markers" / "started").is_file(),
            "release_marker": (work_root / "markers" / "release").is_file(),
            "sampler_ok": bool(sampler is not None and getattr(sampler, "_thread", None) is not None
                                and sampler._thread.is_alive()),
            "attempts_profile": attempts, "proc": proc,
        }
        row[f"drain_{key}_snapshot_json"] = json.dumps(aggregate, sort_keys=True, separators=(",", ":"))
        row[f"drain_{key}_snapshot_count"] = 1
        row[f"drain_{key}_snapshot_capability"] = "available" if proc.get("available") else "unavailable"
        row[f"drain_{key}_snapshot_error"] = None
        if key == "stall":
            row["drain_gdb_status"] = _capture_gdb_snapshot(process.pid, run_dir, attempts) if process is not None else "server_unavailable"
    except Exception:
        row[f"drain_{key}_snapshot_count"] = 0
        row[f"drain_{key}_snapshot_capability"] = "unavailable"
        row[f"drain_{key}_snapshot_error"] = "snapshot_unavailable"


def _capture_stall_snapshot_once(
    tracker: dict[str, bool], row: dict[str, Any], **kwargs: Any,
) -> bool:
    if tracker.get("taken", False):
        return False
    _capture_stall_snapshot(row, **kwargs)
    tracker["taken"] = True
    return True


def _drain_row_defaults() -> dict[str, Any]:
    values: dict[str, Any] = {
        "drain_completed_jobs": 0,
        "drain_remaining_jobs": None,
        "drain_last_progress_elapsed_seconds": None,
        "drain_stalled_seconds": None,
        "drain_health_ok": None,
        "drain_server_alive": None,
        "drain_blocker_marker": None,
        "drain_started_marker": None,
        "drain_release_marker": None,
        "drain_sampler_ok": None,
        "drain_diagnostic_complete": False,
        "drain_diagnostic_error": None,
        "drain_timeout_attempts_profile": None,
        "drain_fixture_started": 0,
        "drain_fixture_finished": 0,
        "drain_fixture_inflight": 0,
        "drain_output_result_files": 0,
        "drain_output_prediction_files": 0,
        "drain_scratch_files": 0,
        "drain_output_file_count": 0,
        "drain_manifest_count": 0,
        "drain_stall_snapshot_count": 0,
        "drain_final_snapshot_count": 0,
        "drain_stall_snapshot_capability": None,
        "drain_final_snapshot_capability": None,
        "drain_stall_snapshot_error": None,
        "drain_final_snapshot_error": None,
        "drain_gdb_status": None,
        "drain_stall_snapshot_json": None,
        "drain_final_snapshot_json": None,
    }
    values.update({f"drain_state_{state}": 0 for state in DRAIN_STATE_NAMES})
    values["drain_state_other"] = 0
    return values


def _update_drain_snapshot(
    row: dict[str, Any], *, attempts: int, drain_started: float,
    completed: int, remaining: int, states: Sequence[str],
    last_progress_at: float, process: subprocess.Popen[bytes] | None,
    sampler: _ProcessSampler | None, work_root: Path,
) -> None:
    counts = _state_counts(states)
    now = time.monotonic()
    row["drain_completed_jobs"] = completed
    row["drain_remaining_jobs"] = remaining
    row["drain_last_progress_elapsed_seconds"] = round(max(0.0, last_progress_at - drain_started), 3)
    row["drain_stalled_seconds"] = round(max(0.0, now - last_progress_at), 3)
    for state, count in counts.items():
        row[f"drain_state_{state}"] = count
    row["drain_server_alive"] = process is not None and process.poll() is None
    markers = work_root / "markers"
    row["drain_blocker_marker"] = (markers / "blocker_claimed").is_file()
    row["drain_started_marker"] = (markers / "started").is_file()
    row["drain_release_marker"] = (markers / "release").is_file()
    row["drain_sampler_ok"] = bool(
        sampler is not None and getattr(sampler, "_thread", None) is not None
        and sampler._thread.is_alive()
    )
    row["drain_timeout_attempts_profile"] = attempts


def _complete_drain_diagnostic(
    row: dict[str, Any], *, host: str, port: int, attempts: int,
    drain_started: float, completed: int, remaining: int,
    states: Sequence[str], last_progress_at: float,
    process: subprocess.Popen[bytes] | None, sampler: _ProcessSampler | None,
    work_root: Path, run_dir: Path | None = None,
) -> None:
    try:
        _update_drain_snapshot(
            row, attempts=attempts, drain_started=drain_started,
            completed=completed, remaining=remaining, states=states,
            last_progress_at=last_progress_at, process=process,
            sampler=sampler, work_root=work_root,
        )
        if run_dir is not None:
            _capture_stall_snapshot(
                row, run_dir=run_dir, host=host, port=port, attempts=attempts,
                drain_started=drain_started, completed=completed, remaining=remaining,
                states=states, last_progress_at=last_progress_at, process=process,
                sampler=sampler, work_root=work_root, kind="final",
            )
        row["drain_health_ok"] = bool(probe_http(host, port, "/health", REQUEST_TIMEOUT_SECONDS).get("ok"))
        row["drain_diagnostic_complete"] = True
        row["drain_diagnostic_error"] = None
    except (OSError, BaselineError, TypeError, ValueError, KeyError):
        row["drain_diagnostic_complete"] = False
        row["drain_diagnostic_error"] = "diagnostic_unavailable"


def _record_sampler_usage(row: dict[str, Any], usage: dict[str, Any]) -> None:
    row["server_cpu_average_percent"] = usage.get("cpu_average_percent")
    row["server_cpu_peak_percent"] = usage.get("cpu_peak_percent")
    row["server_rss_initial_bytes"] = usage.get("rss_initial_bytes")
    row["server_rss_peak_bytes"] = usage.get("rss_peak_bytes")
    row["resource_sample_count"] = usage.get("sample_count")
    row["drain_sampler_ok"] = bool(usage.get("finished") and usage.get("valid"))


def _artifact_payload() -> bytes:
    return b"0 0 0\n"


def _json_response(response: http.client.HTTPResponse) -> tuple[int, dict[str, Any]]:
    body = response.read((1 << 20) + 1)
    content_type = response.headers.get("Content-Type", "").split(";", 1)[0].strip()
    if content_type != "application/json" or len(body) > 1 << 20:
        raise BaselineError("invalid_http_response", "JSON response contract failed")
    try:
        value = json.loads(body.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("invalid_http_response", "JSON response contract failed") from exc
    if not isinstance(value, dict):
        raise BaselineError("invalid_http_response", "JSON response contract failed")
    return int(response.status), value


def _upload_input(host: str, port: int, payload: bytes) -> dict[str, Any]:
    connection = http.client.HTTPConnection(host, port, timeout=REQUEST_TIMEOUT_SECONDS)
    try:
        connection.request(
            "POST", "/api/artifacts/v1/pointclouds", payload,
            {"Content-Type": "text/plain; charset=utf-8", "Connection": "keep-alive"},
        )
        status, body = _json_response(connection.getresponse())
    except (OSError, http.client.HTTPException, TimeoutError) as exc:
        raise BaselineError("artifact_upload_failure", "artifact upload failed") from exc
    finally:
        connection.close()
    if status != 201 or set(body) != {"schema_version", "artifact", "download_url"}:
        raise BaselineError("artifact_upload_contract", "artifact upload contract failed")
    artifact = body.get("artifact")
    if not isinstance(artifact, dict) or set(artifact) != {
        "artifact_id", "sha256", "size_bytes", "kind", "media_type",
        "coordinate_frame", "unit", "point_count",
    }:
        raise BaselineError("artifact_upload_contract", "artifact upload contract failed")
    artifact_id = artifact.get("artifact_id")
    if (
        not isinstance(artifact_id, str) or not INPUT_ARTIFACT_ID.fullmatch(artifact_id)
        or not isinstance(artifact.get("sha256"), str)
        or not re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"])
        or artifact.get("size_bytes") != 12
        or artifact.get("point_count") != 1
        or artifact.get("kind") != "point_cloud"
        or artifact.get("media_type") != "application/vnd.iaisf.pointcloud.xyz-f32le"
        or artifact.get("coordinate_frame") != "camera"
        or artifact.get("unit") != "mm"
        or body.get("download_url") != f"/api/artifacts/v1/files/{artifact_id}"
    ):
        raise BaselineError("artifact_upload_contract", "artifact upload contract failed")
    return artifact


def _submit_on_connection(
    connection: http.client.HTTPConnection, artifact: dict[str, Any]
) -> tuple[str, str, float, str]:
    payload = json.dumps(
        {"schema_version": "1.0", "input_artifacts": [artifact], "requested_outputs": ["segmentation"]},
        separators=(",", ":"),
    ).encode("utf-8")
    started = time.monotonic_ns()
    try:
        connection.request(
            "POST", "/api/weld-inspection/v1/jobs", payload,
            {"Content-Type": "application/json", "Connection": "keep-alive"},
        )
        status, body = _json_response(connection.getresponse())
    except (OSError, http.client.HTTPException, TimeoutError) as exc:
        raise BaselineError("submit_failure", "job submission failed") from exc
    latency = (time.monotonic_ns() - started) / 1_000_000.0
    if status == 202:
        if set(body) != {"job_id", "status_url"}:
            raise BaselineError("submit_contract", "accepted submission contract failed")
        job_id = body.get("job_id")
        status_url = body.get("status_url")
        expected = f"/api/weld-inspection/v1/jobs/{job_id}"
        if not isinstance(job_id, str) or not JOB_ID.fullmatch(job_id) or status_url != expected:
            raise BaselineError("submit_contract", "accepted submission contract failed")
        return job_id, status_url, latency, "accepted"
    if status == 503 and isinstance(body.get("error"), dict):
        error = body["error"]
        if error.get("code") == "queue_full" and isinstance(error.get("message"), str):
            return "", "", latency, "queue_full"
    raise BaselineError("submit_unexpected", "submission returned an unexpected response")


def _submit(host: str, port: int, artifact: dict[str, Any]) -> tuple[str, str, float, str]:
    connection = http.client.HTTPConnection(host, port, timeout=REQUEST_TIMEOUT_SECONDS)
    try:
        return _submit_on_connection(connection, artifact)
    finally:
        connection.close()


def _status(host: str, port: int, status_url: str, expected_job: str) -> str:
    connection = http.client.HTTPConnection(host, port, timeout=REQUEST_TIMEOUT_SECONDS)
    try:
        connection.request("GET", status_url, headers={"Connection": "keep-alive"})
        code, body = _json_response(connection.getresponse())
    except (OSError, http.client.HTTPException, TimeoutError) as exc:
        raise BaselineError("status_failure", "job status request failed") from exc
    finally:
        connection.close()
    if code != 200 or set(body) != {
        "schema_version", "job_id", "application", "phase", "state", "version",
        "created_at", "updated_at", "status_url",
    }:
        raise BaselineError("status_contract", "job status contract failed")
    if (
        body.get("job_id") != expected_job or body.get("application") != "weld_inspection"
        or body.get("phase") != "post_weld" or body.get("status_url") != status_url
        or not isinstance(body.get("state"), str) or body.get("state") not in KNOWN_STATES
    ):
        raise BaselineError("status_contract", "job status contract failed")
    return body["state"]


def _wait_for_state(host: str, port: int, job: str, url: str, expected: set[str], deadline: float) -> str:
    last = ""
    while time.monotonic() < deadline:
        last = _status(host, port, url, job)
        if last in expected:
            return last
        if last in {"failed", "cancelled", "timed_out", "worker_lost"}:
            raise BaselineError("job_failed", "accepted application job failed")
        time.sleep(0.02)
    raise BaselineError("job_timeout", "application job did not reach the expected state")


def _submit_many(host: str, port: int, artifact: dict[str, Any], count: int) -> dict[str, Any]:
    lock = threading.Lock()
    next_index = 0
    submissions: list[tuple[str, str, float, str]] = []
    latencies: list[float] = []
    errors: list[str] = []
    result_lock = threading.Lock()
    barrier = threading.Barrier(SUBMIT_WORKERS)

    def worker() -> None:
        nonlocal next_index
        try:
            barrier.wait(timeout=10.0)
        except threading.BrokenBarrierError:
            return
        connection: http.client.HTTPConnection | None = None
        try:
            while True:
                with lock:
                    if next_index >= count:
                        return
                    next_index += 1
                started = time.monotonic_ns()
                try:
                    if connection is None:
                        connection = http.client.HTTPConnection(
                            host, port, timeout=REQUEST_TIMEOUT_SECONDS
                        )
                    result = _submit_on_connection(connection, artifact)
                    with result_lock:
                        submissions.append(result)
                        latencies.append(result[2])
                except BaselineError as exc:
                    with result_lock:
                        errors.append(exc.category)
                        latencies.append((time.monotonic_ns() - started) / 1_000_000.0)
                    if connection is not None:
                        connection.close()
                        connection = None
        finally:
            if connection is not None:
                connection.close()

    threads = [threading.Thread(target=worker, name=f"iaisf-job-submit-{i}") for i in range(SUBMIT_WORKERS)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    accepted = [item for item in submissions if item[3] == "accepted"]
    queue_full = [item for item in submissions if item[3] == "queue_full"]
    return {
        "accepted": accepted,
        "queue_full": queue_full,
        "all_latencies": latencies,
        "errors": errors,
    }


def _latencies(values: Sequence[float]) -> tuple[float | None, float | None, float | None, float | None]:
    return (
        _percentile(values, 0.50), _percentile(values, 0.95),
        _percentile(values, 0.99), round(max(values), 3) if values else None,
    )


def _effective_config(template: Path, host: str, port: int, work_root: Path, fixture: Path) -> bytes:
    try:
        value = json.loads(template.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("config_invalid", "configuration template is invalid") from exc
    if not isinstance(value, dict):
        raise BaselineError("config_invalid", "configuration template is not an object")
    value.setdefault("server", {})["host"] = host
    value["server"]["port"] = port
    value["server"].setdefault("tcp", {})["input_maximum_capacity_bytes"] = 64 * 1024 * 1024
    value.setdefault("http", {})["body_timeout_ms"] = 300000
    limits = value["http"].setdefault("limits", {})
    limits.update({
        "max_header_bytes": 32 * 1024,
        "max_body_bytes": 64 * 1024 * 1024 - 32 * 1024,
        "max_response_body_bytes": 1 * 1024 * 1024,
        "max_routes": 256,
    })
    artifact_root = work_root / "artifacts"
    scratch_root = work_root / "scratch"
    output_root = work_root / "outputs"
    for directory in (artifact_root, scratch_root, output_root, work_root / "markers"):
        directory.mkdir(parents=True, exist_ok=True)
    for path in (work_root / "engine.bin", work_root / "plugin.so"):
        path.write_bytes(b"synthetic\n")
    value["applications"] = {
        "enabled": True,
        "artifact_root": str(artifact_root),
        "scratch_root": str(scratch_root),
        "output_root": str(output_root),
        "repository_capacity": REPOSITORY_CAPACITY,
        "queue_capacity": QUEUE_CAPACITY,
        "ptv2": {
            "executable": str(fixture.resolve()),
            "working_directory": str(work_root.resolve()),
            "engine": str((work_root / "engine.bin").resolve()),
            "plugin": str((work_root / "plugin.so").resolve()),
            "timeout_ms": 60000,
        },
        "weld_agent": {
            "python_executable": str(fixture.resolve()),
            "project_root": str(work_root.resolve()),
            "orchestrator": str(fixture.resolve()),
            "tool_config": str((work_root / "tool-config.json").resolve()),
            "timeout_ms": 1000,
        },
    }
    value.setdefault("metrics", {})["enabled"] = False
    value.setdefault("diagnostics", {})["enabled"] = False
    return json.dumps(value, sort_keys=True, indent=2).encode("utf-8") + b"\n"


def _run_profile(*, repo_root: Path, server_path: Path, template: Path, fixture: Path,
                 run_dir: Path, host: str, port: int, attempts: int) -> tuple[dict[str, Any], dict[str, Any]]:
    # Keep adapter-owned paths outside /mnt/<drive> on WSL.  The production
    # adapter intentionally converts those paths for Windows child processes;
    # this Linux-only synthetic executable must receive native Linux paths.
    work_root = Path(tempfile.mkdtemp(prefix=f"iaisf-job-stress-{attempts}-"))
    fixture_copy: Path | None = None
    process: subprocess.Popen[bytes] | None = None
    captures: list[_PipeCapture] = []
    sampler: _ProcessSampler | None = None
    cleanup = {"process_reaped": True, "process_group_reaped": True,
               "capture_threads_finished": True, "port_released": None,
               "temporary_config_removed": False, "work_root_removed": False,
               "markers_removed": False, "artifacts_removed": False,
               "fixture_copy_removed": True}
    row: dict[str, Any] = {"scenario": SCENARIO, "job_attempts": attempts,
                           "submit_workers": SUBMIT_WORKERS, "queue_capacity": QUEUE_CAPACITY,
                           "repository_capacity": REPOSITORY_CAPACITY, "failure_category": None}
    row.update(_drain_row_defaults())
    try:
        validate_loopback_host(host)
        validate_port(port)
        if not server_path.is_file() or server_path.is_symlink():
            raise BaselineError("executable_missing", "required benchmark executable is unavailable")
        if not port_is_available(host, port):
            raise BaselineError("port_in_use", "benchmark port is unavailable")
        fixture_copy = _copy_fixture_to_work_root(fixture, work_root)
        cleanup["fixture_copy_removed"] = False
        config = _effective_config(template, host, port, work_root, fixture_copy)
        config_path = work_root / "config.json"
        atomic_write_bytes(config_path, config)
        cleanup["temporary_config_removed"] = False
        process = subprocess.Popen(
            [str(server_path.resolve()), "--serve", "--config", str(config_path.resolve())],
            cwd=str(repo_root), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, shell=False, start_new_session=(os.name == "posix"),
        )
        assert process.stdout is not None and process.stderr is not None
        captures = [_PipeCapture(process.stdout), _PipeCapture(process.stderr)]
        for capture in captures:
            capture.start()
        deadline = time.monotonic() + START_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise BaselineError("process_exit", "service exited before becoming ready")
            probe = probe_http(host, port, "/health", REQUEST_TIMEOUT_SECONDS)
            if probe["ok"]:
                break
            time.sleep(0.05)
        else:
            raise BaselineError("startup_timeout", "service did not become ready")

        artifact = _upload_input(host, port, _artifact_payload())
        blocker = _submit(host, port, artifact)
        if blocker[3] != "accepted":
            raise BaselineError("blocker_submit", "blocker job was not accepted")
        blocker_job, blocker_url = blocker[0], blocker[1]
        marker = work_root / "markers" / "started"
        started_deadline = time.monotonic() + START_TIMEOUT_SECONDS
        while time.monotonic() < started_deadline:
            if marker.exists() and _status(host, port, blocker_url, blocker_job) == "running":
                break
            time.sleep(0.02)
        else:
            raise BaselineError("blocker_not_running", "blocker job did not reach running")

        initial_usage = _read_process_usage(process.pid)
        if initial_usage is None:
            raise BaselineError("resource_sampling_failure", "initial service resource sample unavailable")
        sampler = _ProcessSampler(process.pid, initial_usage)
        sampler.start()
        client_cpu_start = time.process_time()
        batch_submit_started = time.monotonic()
        remaining = _submit_many(host, port, artifact, attempts - 1)
        batch_submit_finished = time.monotonic()
        measured_submit_requests, batch_submission_duration, batch_submission_rate = _batch_submission_metrics(
            attempts - 1, batch_submit_started, batch_submit_finished
        )
        all_results = remaining
        accepted = [(blocker_job, blocker_url, blocker[2], blocker[3])] + all_results["accepted"]
        queue_full = all_results["queue_full"]
        unexpected = all_results["errors"]
        row.update({
            "accepted": len(accepted), "queue_full": len(queue_full),
            "unexpected_failures": len(unexpected),
            "blocker_submit_latency_ms": _finite(round(blocker[2], 3)),
            "measured_submit_requests": measured_submit_requests,
            "batch_submission_duration_seconds": round(batch_submission_duration, 3),
            "batch_submission_requests_per_second": _finite(round(batch_submission_rate, 3)),
        })
        if unexpected or len(all_results["all_latencies"]) != attempts - 1:
            raise BaselineError("unexpected_submission_failure", "unexpected submission response")
        expected_accepted = min(attempts, QUEUE_CAPACITY + 1)
        if len(accepted) != expected_accepted or len(queue_full) != attempts - expected_accepted:
            raise BaselineError("backpressure_mismatch", "accepted and queue_full counts are invalid")
        accepted_urls = [(job, url) for job, url, _, _ in accepted]
        states_deadline = time.monotonic() + START_TIMEOUT_SECONDS
        before_states: list[str] = []
        while time.monotonic() < states_deadline:
            before_states = [_status(host, port, url, job) for job, url in accepted_urls]
            if before_states.count("running") == 1 and before_states.count("queued") == len(accepted) - 1:
                break
            time.sleep(0.02)
        if before_states.count("running") != 1 or before_states.count("queued") != len(accepted) - 1:
            raise BaselineError("queue_state_mismatch", "release-time queue state is invalid")
        row.update({
            "running_before_release": before_states.count("running"),
            "queued_before_release": before_states.count("queued"),
        })

        release_path = work_root / "markers" / "release"
        with release_path.open("x", encoding="ascii") as stream:
            stream.write("release\n")
        drain_started = time.monotonic()
        pending = list(accepted_urls)
        state_by_job = {item: state for item, state in zip(accepted_urls, before_states)}
        last_progress_at = drain_started
        _update_drain_snapshot(
            row, attempts=attempts, drain_started=drain_started,
            completed=0, remaining=len(pending), states=state_by_job.values(),
            last_progress_at=last_progress_at, process=process,
            sampler=sampler, work_root=work_root,
        )
        deadline = time.monotonic() + DRAIN_TIMEOUT_SECONDS
        stall_snapshot_state: dict[str, bool] = {"taken": False}
        while pending and time.monotonic() < deadline:
            next_pending: list[tuple[str, str]] = []
            for job, url in pending:
                state = _status(host, port, url, job)
                state_by_job[(job, url)] = state
                if state == "succeeded":
                    continue
                if state in {"failed", "cancelled", "timed_out", "worker_lost"}:
                    _complete_drain_diagnostic(
                        row, host=host, port=port, attempts=attempts,
                        drain_started=drain_started,
                        completed=sum(state == "succeeded" for state in state_by_job.values()),
                        remaining=sum(state != "succeeded" for state in state_by_job.values()),
                        states=state_by_job.values(),
                        last_progress_at=last_progress_at, process=process,
                        sampler=sampler, work_root=work_root, run_dir=run_dir,
                    )
                    raise BaselineError("job_failed", "accepted job did not succeed")
                next_pending.append((job, url))
            completed = len(accepted) - len(next_pending)
            if completed > row.get("drain_completed_jobs", 0):
                last_progress_at = time.monotonic()
            pending = next_pending
            _update_drain_snapshot(
                row, attempts=attempts, drain_started=drain_started,
                completed=completed, remaining=len(pending), states=state_by_job.values(),
                last_progress_at=last_progress_at, process=process,
                sampler=sampler, work_root=work_root,
            )
            if pending and time.monotonic() - last_progress_at >= 5.0:
                _capture_stall_snapshot_once(
                    stall_snapshot_state, row, run_dir=run_dir, host=host, port=port,
                    attempts=attempts, drain_started=drain_started, completed=completed,
                    remaining=len(pending), states=state_by_job.values(),
                    last_progress_at=last_progress_at, process=process, sampler=sampler,
                    work_root=work_root, kind="stall",
                )
            if pending:
                time.sleep(0.02)
        if pending:
            _capture_stall_snapshot_once(
                stall_snapshot_state, row, run_dir=run_dir, host=host, port=port,
                attempts=attempts, drain_started=drain_started,
                completed=len(accepted) - len(pending), remaining=len(pending),
                states=state_by_job.values(), last_progress_at=last_progress_at,
                process=process, sampler=sampler, work_root=work_root, kind="stall",
            )
            _complete_drain_diagnostic(
                row, host=host, port=port, attempts=attempts,
                drain_started=drain_started, completed=len(accepted) - len(pending),
                remaining=len(pending), states=state_by_job.values(),
                last_progress_at=last_progress_at, process=process,
                sampler=sampler, work_root=work_root, run_dir=run_dir,
            )
            raise BaselineError("drain_timeout", "accepted jobs did not drain")
        drain_duration = max(0.001, time.monotonic() - drain_started)
        row["drain_diagnostic_complete"] = True
        row["drain_diagnostic_error"] = None

        recovery = _submit(host, port, artifact)
        if recovery[3] != "accepted":
            raise BaselineError("recovery_rejected", "recovery job was not accepted")
        recovery_state = _wait_for_state(
            host, port, recovery[0], recovery[1], {"succeeded"},
            time.monotonic() + DRAIN_TIMEOUT_SECONDS,
        )
        client_cpu_seconds = max(0.0, time.process_time() - client_cpu_start)
        usage = sampler.stop()
        sampler = None
        _record_sampler_usage(row, usage)
        if not usage.get("finished") or not usage.get("valid"):
            raise BaselineError("resource_sampling_failure", "resource sampler did not produce valid windows")
        health = probe_http(host, port, "/health", REQUEST_TIMEOUT_SECONDS)
        if not health["ok"]:
            row["drain_health_ok"] = False
            raise BaselineError("post_load_health_failure", "post-load health contract failed")
        row["drain_health_ok"] = True
        row["drain_diagnostic_complete"] = True
        all_latencies = [blocker[2]] + all_results["all_latencies"]
        accepted_latencies = [item[2] for item in accepted]
        queue_latencies = [item[2] for item in queue_full]
        all_p50, all_p95, all_p99, all_max = _latencies(all_latencies)
        acc_p50, acc_p95, acc_p99, _ = _latencies(accepted_latencies)
        qf_p50, qf_p95, qf_p99, _ = _latencies(queue_latencies)
        row.update({
            "accepted": len(accepted), "queue_full": len(queue_full), "unexpected_failures": 0,
            "blocker_submit_latency_ms": _finite(round(blocker[2], 3)),
            "measured_submit_requests": measured_submit_requests,
            "batch_submission_duration_seconds": round(batch_submission_duration, 3),
            "batch_submission_requests_per_second": _finite(round(batch_submission_rate, 3)),
            "all_latency_p50_ms": _finite(all_p50), "all_latency_p95_ms": _finite(all_p95),
            "all_latency_p99_ms": _finite(all_p99), "all_latency_max_ms": _finite(all_max),
            "accepted_latency_p50_ms": _finite(acc_p50), "accepted_latency_p95_ms": _finite(acc_p95),
            "accepted_latency_p99_ms": _finite(acc_p99), "queue_full_latency_p50_ms": _finite(qf_p50),
            "queue_full_latency_p95_ms": _finite(qf_p95), "queue_full_latency_p99_ms": _finite(qf_p99),
            "running_before_release": before_states.count("running"),
            "queued_before_release": before_states.count("queued"),
            "drain_duration_seconds": round(drain_duration, 3),
            "drain_jobs_per_second": _finite(round(len(accepted) / drain_duration, 3)),
            "succeeded": len(accepted), "recovery_accepted": True, "recovery_succeeded": recovery_state == "succeeded",
            "post_load_health_ok": True, "server_cpu_average_percent": usage.get("cpu_average_percent"),
            "server_cpu_peak_percent": usage.get("cpu_peak_percent"), "server_rss_initial_bytes": usage.get("rss_initial_bytes"),
            "server_rss_peak_bytes": usage.get("rss_peak_bytes"), "resource_sample_count": usage.get("sample_count"),
            "client_cpu_seconds": _finite(round(client_cpu_seconds, 3)), "failure_category": None,
        })
    except BaselineError as exc:
        row["failure_category"] = exc.category
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError):
        row["failure_category"] = "internal_failure"
    finally:
        if sampler is not None:
            try:
                usage = sampler.stop()
                _record_sampler_usage(row, usage)
                sampler = None
            except Exception:
                row["failure_category"] = row.get("failure_category") or "resource_sampling_failure"
        if process is not None:
            terminated = _terminate_process(process)
            cleanup["process_reaped"] = bool(process.poll() is not None)
            cleanup["process_group_reaped"] = _process_group_reaped(process)
            if not terminated:
                cleanup["process_group_reaped"] = False
        for capture in captures:
            cleanup["capture_threads_finished"] = bool(capture.join()) and cleanup["capture_threads_finished"]
            if capture.overflow:
                row["failure_category"] = row.get("failure_category") or "process_output_limit"
        try:
            config_path = work_root / "config.json"
            config_path.unlink(missing_ok=True)
            cleanup["temporary_config_removed"] = not config_path.exists()
        except OSError:
            cleanup["temporary_config_removed"] = False
        try:
            shutil.rmtree(work_root)
            cleanup["work_root_removed"] = not work_root.exists()
        except OSError:
            cleanup["work_root_removed"] = False
        cleanup["markers_removed"] = cleanup["work_root_removed"]
        cleanup["artifacts_removed"] = cleanup["work_root_removed"]
        cleanup["fixture_copy_removed"] = (
            fixture_copy is None
            or (not fixture_copy.exists() and not fixture_copy.is_symlink())
        )
        try:
            cleanup["port_released"] = port_is_available(host, port)
        except Exception:
            cleanup["port_released"] = False
        if row.get("failure_category") is None and not all(value is True for value in cleanup.values() if value is not None):
            row["failure_category"] = "cleanup_failure"
    return row, cleanup


def run_application_job_stress(*, repo_root: Path, server_path: Path, config_template: Path,
                               fixture: Path, output_root: Path, host: str = "127.0.0.1",
                               allow_dirty: bool = False) -> tuple[Path, dict[str, Any]]:
    if os.name != "posix":
        raise BaselineError("platform_unsupported", "application job stress requires Linux process groups")
    attempts = _validate_profiles(PROFILE_ATTEMPTS)
    git = git_snapshot(repo_root)
    if git["dirty"] and not allow_dirty:
        raise BaselineError("git_dirty", "working tree is dirty")
    if not server_path.is_file() or server_path.is_symlink():
        raise BaselineError("executable_missing", "required benchmark executable is unavailable")
    _validate_fixture_executable(fixture)
    run_dir, actual_run_id = create_unique_run_directory(output_root, make_run_id(git["sha"], SCENARIO))
    rows: list[dict[str, Any]] = []
    cleanup_rows: list[dict[str, Any]] = []
    started = utc_now()
    try:
        for index, count in enumerate(attempts):
            row, cleanup = _run_profile(
                repo_root=repo_root, server_path=server_path, template=config_template,
                fixture=fixture, run_dir=run_dir, host=host, port=19000 + index, attempts=count,
            )
            rows.append(row)
            cleanup_rows.append(cleanup)
        failure = next((row.get("failure_category") for row in rows if row.get("failure_category")), None)
        outcome = "success" if failure is None else "failure"
        summary = {
            "schema_version": SCHEMA_VERSION, "run_id": actual_run_id,
            "scenario": SCENARIO, "started_at_utc": started, "finished_at_utc": utc_now(),
            "outcome": outcome, "failure_category": failure, "profiles": rows,
            "method": _method_metadata(),
            "cleanup": {"all_profiles_clean": all(all(value is True for value in item.values() if value is not None) for item in cleanup_rows),
                        "profile_count": len(rows)},
        }
        manifest = {
            "schema_version": SCHEMA_VERSION, "run_id": actual_run_id,
            "scenario": SCENARIO, "started_at_utc": started,
            "finished_at_utc": summary["finished_at_utc"], "git_sha": git["sha"],
            "git_dirty": git["dirty"], "build_configuration": "Release",
            "environment": environment_snapshot(),
            "method": _method_metadata(),
            "parameters": {"host": host, "job_attempts": list(attempts), "submit_workers": SUBMIT_WORKERS,
                            "queue_capacity": QUEUE_CAPACITY, "repository_capacity": REPOSITORY_CAPACITY,
                            "sampler_interval_seconds": SAMPLER_INTERVAL_SECONDS,
                            "cpu_percent_definition": "100% = one logical CPU core",
                            "load_generator": "Python standard-library http.client",
                            "client_server_same_host": True, "synthetic_adapter": True},
        }
        atomic_write_json(run_dir / "manifest.json", manifest)
        atomic_write_json(run_dir / "summary.json", summary)
        atomic_write_csv(run_dir / "profiles.csv", rows, CSV_FIELDS)
        atomic_write_bytes(run_dir / "run.log", ("application job stress run\nsynthetic adapter / framework queue stress\n" + f"profiles={len(rows)}\noutcome={outcome}\n").encode("utf-8"))
        return run_dir, summary
    except Exception:
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="build/linux-release/iaisf_server")
    parser.add_argument("--config-template", default="benchmarks/configs/baseline-smoke.json")
    parser.add_argument("--fixture", default="benchmarks/fixtures/mock_ptv2_cli.py")
    parser.add_argument("--output-root", default="benchmarks/results")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--allow-dirty", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        run_dir, summary = run_application_job_stress(
            repo_root=Path.cwd(), server_path=Path(args.server), config_template=Path(args.config_template),
            fixture=Path(args.fixture), output_root=Path(args.output_root), host=args.host,
            allow_dirty=args.allow_dirty,
        )
    except BaselineError as exc:
        print(f"benchmark failed: {exc.category}", file=sys.stderr)
        return 2
    print(json.dumps({"run_directory": run_dir.name, "outcome": summary["outcome"]}, sort_keys=True))
    return 0 if summary["outcome"] == "success" else 1


if __name__ == "__main__":
    raise SystemExit(main())
