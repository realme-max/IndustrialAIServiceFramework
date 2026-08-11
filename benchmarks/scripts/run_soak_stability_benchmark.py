#!/usr/bin/env python3
"""Run a serial, real PTV2/WeldAgent stability benchmark.

The benchmark reuses the existing real-AI HTTP contracts.  It is deliberately
serial: one service, one worker sequence, two jobs per cycle, and no overlap.
It measures stability and resource trends, not throughput or AI performance.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

try:
    from benchmark_common import (
        BaselineError,
        SCHEMA_VERSION,
        atomic_write_bytes,
        atomic_write_json,
        create_unique_run_directory,
        environment_snapshot,
        git_snapshot,
        make_run_id,
        port_is_available,
        sha256_file,
        utc_now,
        validate_loopback_host,
        validate_port,
    )
    from run_baseline_smoke import _PipeCapture, _process_group_reaped, _terminate_process
    from run_real_ai_latency_benchmark import (
        _health,
        _input_spec,
        _prepare_config,
        _prepare_runtime,
        _run_one_job,
        _upload,
        _write_xyz_text,
    )
    from run_http_benchmark import _read_process_usage
except ImportError:  # pragma: no cover
    from .benchmark_common import (
        BaselineError,
        SCHEMA_VERSION,
        atomic_write_bytes,
        atomic_write_json,
        create_unique_run_directory,
        environment_snapshot,
        git_snapshot,
        make_run_id,
        port_is_available,
        sha256_file,
        utc_now,
        validate_loopback_host,
        validate_port,
    )
    from .run_baseline_smoke import _PipeCapture, _process_group_reaped, _terminate_process
    from .run_real_ai_latency_benchmark import (
        _health,
        _input_spec,
        _prepare_config,
        _prepare_runtime,
        _run_one_job,
        _upload,
        _write_xyz_text,
    )
    from .run_http_benchmark import _read_process_usage


SCENARIO = "soak-stability"
CADENCE_SECONDS = 120
ALLOWED_DURATIONS = (600, 7200, 43200)
RESOURCE_INTERVAL_SECONDS = 10.0
START_TIMEOUT_SECONDS = 30.0
CSV_FIELDS = (
    "record_type", "recorded_at_utc", "elapsed_seconds", "cycle_index", "application",
    "schedule_lag_seconds", "submit_latency_ms", "terminal_latency_ms",
    "result_latency_ms", "download_latency_ms", "total_latency_ms", "final_state",
    "artifact_verified", "health_ok", "server_pid_same", "external_child_count",
    "cpu_seconds", "rss_bytes", "thread_count", "fd_count", "runtime_disk_bytes",
    "failure_category",
)


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
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return round(ordered[low] + (ordered[high] - ordered[low]) * (position - low), 3)


def _latency_stats(values: Sequence[float]) -> dict[str, float | None]:
    if any(not math.isfinite(float(value)) or value < 0 for value in values):
        raise BaselineError("invalid_measurement", "latency sample is invalid")
    return {
        "min_ms": round(min(values), 3) if values else None,
        "mean_ms": round(sum(values) / len(values), 3) if values else None,
        "p50_ms": _percentile(values, 0.50),
        "p95_ms": _percentile(values, 0.95),
        "p99_ms": _percentile(values, 0.99),
        "max_ms": round(max(values), 3) if values else None,
    }


def _validate_duration_and_cadence(duration_seconds: int, cadence_seconds: int) -> int:
    if duration_seconds not in ALLOWED_DURATIONS:
        raise BaselineError("invalid_duration", "duration is outside the supported stages")
    if cadence_seconds != CADENCE_SECONDS or duration_seconds % cadence_seconds:
        raise BaselineError("invalid_cadence", "cadence must be the fixed 120 second interval")
    return duration_seconds // cadence_seconds


def _directory_size(root: Path) -> int:
    total = 0
    try:
        stack = [root]
        visited = 0
        while stack:
            current = stack.pop()
            with os.scandir(current) as entries:
                for entry in entries:
                    visited += 1
                    if visited > 100_000:
                        raise BaselineError("resource_sampling_failure", "runtime directory is too large")
                    if entry.is_symlink():
                        continue
                    if entry.is_dir(follow_symlinks=False):
                        stack.append(Path(entry.path))
                    elif entry.is_file(follow_symlinks=False):
                        total += entry.stat(follow_symlinks=False).st_size
    except FileNotFoundError:
        return 0
    except BaselineError:
        raise
    except OSError as exc:
        raise BaselineError("resource_sampling_failure", "runtime disk usage is unavailable") from exc
    return total


def _direct_child_pids(pid: int) -> list[int]:
    children: set[int] = set()
    task_root = Path(f"/proc/{pid}/task")
    try:
        task_dirs = list(task_root.iterdir())
        for task in task_dirs:
            child_file = task / "children"
            if child_file.is_file():
                for token in child_file.read_text(encoding="ascii").split():
                    if token.isdigit():
                        children.add(int(token))
    except (OSError, UnicodeError, ValueError):
        return []
    return sorted(children)


def _process_snapshot(pid: int, runtime_root: Path) -> dict[str, Any] | None:
    usage = _read_process_usage(pid)
    if usage is None:
        return None
    try:
        thread_count = len(list(Path(f"/proc/{pid}/task").iterdir()))
        fd_count = len(list(Path(f"/proc/{pid}/fd").iterdir()))
        disk_bytes = _directory_size(runtime_root)
    except (OSError, BaselineError):
        return None
    return {
        "cpu_seconds": float(usage[0]),
        "rss_bytes": int(usage[1]),
        "thread_count": thread_count,
        "fd_count": fd_count,
        "runtime_disk_bytes": disk_bytes,
        "external_child_count": len(_direct_child_pids(pid)),
    }


class _SampleWriter:
    """Append-only UTF-8 CSV writer with a fixed header and immediate flush."""

    def __init__(self, path: Path) -> None:
        if path.exists() or path.is_symlink():
            raise BaselineError("output_exists", "sample file already exists")
        try:
            self._stream = path.open("x", encoding="utf-8", newline="")
            self._writer = csv.DictWriter(self._stream, fieldnames=CSV_FIELDS, extrasaction="raise")
            self._writer.writeheader()
            self._stream.flush()
        except (OSError, csv.Error) as exc:
            raise BaselineError("output_write_failure", "unable to create sample file") from exc
        self._lock = threading.Lock()
        self.error: str | None = None

    def append(self, row: Mapping[str, Any]) -> None:
        normalized = {
            field: "" if row.get(field) is None else "true" if row.get(field) is True
            else "false" if row.get(field) is False else row.get(field)
            for field in CSV_FIELDS
        }
        try:
            with self._lock:
                self._writer.writerow(normalized)
                self._stream.flush()
        except (OSError, csv.Error, ValueError):
            self.error = "output_write_failure"
            raise BaselineError("output_write_failure", "unable to append sample")

    def close(self) -> bool:
        try:
            with self._lock:
                self._stream.flush()
                self._stream.close()
            return True
        except OSError:
            return False


class _ResourceSampler:
    def __init__(
        self,
        pid: int,
        runtime_root: Path,
        origin: float,
        on_sample: Callable[[float, Mapping[str, Any]], None],
        *,
        interval_seconds: float = RESOURCE_INTERVAL_SECONDS,
    ) -> None:
        self.pid = pid
        self.runtime_root = runtime_root
        self.origin = origin
        self.interval_seconds = interval_seconds
        self._on_sample = on_sample
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="iaisf-soak-resource")
        self._error: str | None = None
        self._sample_count = 0

    def start(self) -> None:
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.wait(self.interval_seconds):
            snapshot = _process_snapshot(self.pid, self.runtime_root)
            if snapshot is None:
                self._error = "resource_sampling_failure"
                continue
            try:
                self._on_sample(time.monotonic() - self.origin, snapshot)
                self._sample_count += 1
            except BaselineError as exc:
                self._error = exc.category

    def stop(self) -> dict[str, Any]:
        self._stop.set()
        self._thread.join(timeout=5.0)
        return {
            "finished": not self._thread.is_alive(),
            "sample_count": self._sample_count,
            "error_category": self._error,
        }


def _resource_summary(samples: Sequence[Mapping[str, Any]], duration_seconds: float) -> dict[str, Any]:
    if not samples:
        raise BaselineError("resource_sampling_failure", "no resource samples were captured")
    required = ("cpu_seconds", "rss_bytes", "thread_count", "fd_count", "runtime_disk_bytes")
    for sample in samples:
        for field in required:
            if not isinstance(sample.get(field), (int, float)) or not math.isfinite(float(sample[field])):
                raise BaselineError("resource_sampling_failure", "resource sample is invalid")

    elapsed = max(float(duration_seconds), 1.0) / 3600.0

    def trend(field: str) -> dict[str, Any]:
        values = [float(item[field]) for item in samples]
        first = values[0]
        last = values[-1]
        return {
            "initial": int(first) if field != "cpu_seconds" else round(first, 3),
            "final": int(last) if field != "cpu_seconds" else round(last, 3),
            "min": int(min(values)) if field != "cpu_seconds" else round(min(values), 3),
            "max": int(max(values)) if field != "cpu_seconds" else round(max(values), 3),
            "first_hour_median": round(statistics.median(values[: max(1, sum(float(x.get("elapsed_seconds", 0)) <= 3600 for x in samples))]), 3),
            "last_hour_median": round(statistics.median(values[-max(1, sum(float(x.get("elapsed_seconds", 0)) >= max(0.0, duration_seconds - 3600) for x in samples)):]), 3),
            "per_hour_change": round((last - first) / elapsed, 3),
        }

    return {
        "sample_count": len(samples),
        "cpu_seconds": trend("cpu_seconds"),
        "rss_bytes": trend("rss_bytes"),
        "thread_count": trend("thread_count"),
        "fd_count": trend("fd_count"),
        "runtime_disk_bytes": trend("runtime_disk_bytes"),
        "external_child_count_max": max(int(item.get("external_child_count", 0)) for item in samples),
    }


def _prepare_soak_config(
    template: Path,
    host: str,
    port: int,
    work_root: Path,
    runtime_root: Path,
) -> bytes:
    """Use the existing service config preparation with the soak repository cap."""
    prepared = _prepare_config(template, host, port, work_root, runtime_root)
    try:
        value = json.loads(prepared.decode("utf-8"))
        applications = value["applications"]
        applications["repository_capacity"] = 1024
        return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("utf-8")
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise BaselineError("config_invalid", "prepared application configuration is invalid") from exc


def _job_latency_summary(records: Sequence[Mapping[str, Any]], application: str, duration_seconds: float) -> dict[str, Any]:
    selected = [item for item in records if item.get("application") == application and item.get("record_type") == "job"]
    result: dict[str, Any] = {"jobs": len(selected), "successes": sum(item.get("final_state") == "succeeded" for item in selected), "failures": sum(item.get("final_state") != "succeeded" for item in selected)}
    for field in ("submit_latency_ms", "terminal_latency_ms", "result_latency_ms", "download_latency_ms", "total_latency_ms"):
        values = [float(item[field]) for item in selected if isinstance(item.get(field), (int, float))]
        result[field] = _latency_stats(values)
        hour = [float(item[field]) for item in selected if float(item.get("elapsed_seconds", 0)) <= 3600 and isinstance(item.get(field), (int, float))]
        tail = [float(item[field]) for item in selected if float(item.get("elapsed_seconds", 0)) >= max(0.0, duration_seconds - 3600) and isinstance(item.get(field), (int, float))]
        result[field + "_first_hour"] = _latency_stats(hour)
        result[field + "_last_hour"] = _latency_stats(tail)
    return result


def _append_event(log_lines: list[str], event: str, **fields: object) -> None:
    safe = [event]
    for key, value in fields.items():
        if key in {"job_id", "url", "path", "command", "stdout", "stderr"}:
            continue
        text = str(value)
        if any(ord(char) < 0x20 for char in text) or len(text) > 80:
            text = "bounded"
        safe.append(f"{key}={text}")
    log_lines.append(" ".join(safe)[:200])


def _run_soak(
    *,
    repo_root: Path,
    server_path: Path,
    config_template: Path,
    ptv2_input: Path,
    weldagent_input: Path,
    weldagent_runtime: Path,
    weldagent_tool_config: Path,
    output_root: Path,
    duration_seconds: int,
    cadence_seconds: int,
    host: str = "127.0.0.1",
    port: int = 18490,
    allow_dirty: bool = False,
) -> tuple[Path, dict[str, Any]]:
    if os.name != "posix":
        raise BaselineError("platform_unsupported", "soak benchmark requires Linux")
    expected_cycles = _validate_duration_and_cadence(duration_seconds, cadence_seconds)
    validate_loopback_host(host)
    validate_port(port)
    git = git_snapshot(repo_root)
    if git["dirty"] and not allow_dirty:
        raise BaselineError("git_dirty", "working tree is dirty")
    if not server_path.is_file() or server_path.is_symlink() or not os.access(server_path, os.X_OK):
        raise BaselineError("server_missing", "Linux Release server is unavailable")
    if not config_template.is_file() or config_template.is_symlink():
        raise BaselineError("config_missing", "configuration template is unavailable")
    if not port_is_available(host, port):
        raise BaselineError("port_in_use", "benchmark port is unavailable")
    pt_spec = _input_spec(ptv2_input, "8c9bd45f520f4e85e914f1628ca2d366fb6983a917a0ea7818a3865e8ae8c8ea", 24576, 2048)
    wg_spec = _input_spec(weldagent_input, "40ea2c408eeb082559b706929161dba1d979f3e3634b24580e90fe94bc9806e7", 9877368, 823114)
    run_dir, run_id = create_unique_run_directory(output_root, make_run_id(git["sha"], SCENARIO))
    work_root = Path(tempfile.mkdtemp(prefix=".soak-", dir=str(output_root)))
    runtime_root = work_root / "weld-agent-runtime"
    config_path = work_root / "config.json"
    samples_path = run_dir / "samples.csv"
    writer: _SampleWriter | None = None
    process: subprocess.Popen[bytes] | None = None
    captures: list[_PipeCapture] = []
    sampler: _ResourceSampler | None = None
    resource_samples: list[dict[str, Any]] = []
    job_records: list[dict[str, Any]] = []
    cycle_records: list[dict[str, Any]] = []
    log_lines: list[str] = []
    cleanup = {
        "process_reaped": True, "process_group_reaped": True, "capture_threads_finished": True,
        "temporary_config_removed": False, "runtime_copy_removed": False,
        "work_root_removed": False, "port_released": False, "result_tmp_files_removed": True,
    }
    started_at = utc_now()
    origin = time.monotonic()
    process_pid: int | None = None
    failure_category: str | None = None
    failure_stage: str | None = None
    failure_cycle: int | None = None
    failure_application: str | None = None
    stop_requested = False

    def append_resource(elapsed: float, snapshot: Mapping[str, Any]) -> None:
        row = {"record_type": "resource", "recorded_at_utc": utc_now(), "elapsed_seconds": round(elapsed, 3), **snapshot}
        resource_samples.append(row)
        if writer is not None:
            writer.append(row)

    try:
        writer = _SampleWriter(samples_path)
        _append_event(log_lines, "run_created", scenario=SCENARIO)
        _prepare_runtime(weldagent_runtime, weldagent_tool_config, runtime_root)
        for directory in (work_root / "artifact-root", work_root / "scratch-root", work_root / "output-root"):
            directory.mkdir(parents=True, exist_ok=False)
        config_bytes = _prepare_soak_config(config_template, host, port, work_root, runtime_root)
        atomic_write_bytes(config_path, config_bytes)
        process = subprocess.Popen(
            [str(server_path.resolve()), "--serve", "--config", str(config_path.resolve())],
            cwd=str(repo_root), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, shell=False, start_new_session=True,
        )
        process_pid = process.pid
        assert process.stdout is not None and process.stderr is not None
        captures = [_PipeCapture(process.stdout), _PipeCapture(process.stderr)]
        for capture in captures:
            capture.start()
        deadline = time.monotonic() + START_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise BaselineError("server_exit", "server exited before becoming ready")
            try:
                if _health(host, port):
                    break
            except BaselineError:
                pass
            time.sleep(0.1)
        else:
            raise BaselineError("server_start_timeout", "server did not become ready")
        if process.poll() is not None or process.pid != process_pid:
            raise BaselineError("server_pid_changed", "server process identity changed")

        pt_text = work_root / "ptv2-input.txt"
        wg_text = work_root / "weldagent-input.txt"
        _write_xyz_text(pt_spec["path"], pt_text, pt_spec["point_count"])
        _write_xyz_text(wg_spec["path"], wg_text, wg_spec["point_count"])
        pt_artifact, _ = _upload(host, port, pt_text, pt_spec["canonical_size_bytes"], pt_spec["point_count"])
        wg_artifact, _ = _upload(host, port, wg_text, wg_spec["canonical_size_bytes"], wg_spec["point_count"])
        initial = _process_snapshot(process.pid, work_root)
        if initial is None:
            raise BaselineError("resource_sampling_failure", "initial resource sample unavailable")
        append_resource(0.0, initial)
        sampler = _ResourceSampler(process.pid, work_root, origin, append_resource)
        sampler.start()

        for cycle_index in range(expected_cycles):
            scheduled = origin + cycle_index * cadence_seconds
            while True:
                remaining = scheduled - time.monotonic()
                if remaining <= 0:
                    break
                time.sleep(min(remaining, 1.0))
            cycle_started = time.monotonic()
            lag = max(0.0, cycle_started - scheduled)
            _append_event(log_lines, "cycle_started", cycle=cycle_index + 1)
            cycle_ok = True
            cycle_health = False
            cycle_failure: str | None = None
            for application, artifact, point_count in (
                ("weld_inspection", pt_artifact, pt_spec["point_count"]),
                ("welding_guidance", wg_artifact, wg_spec["point_count"]),
            ):
                failure_application = application
                try:
                    measurement = _run_one_job(host, port, application, artifact, point_count)
                    children = _direct_child_pids(process.pid)
                    if process.poll() is not None or children:
                        raise BaselineError("child_process_leak", "job left an unexpected child process")
                    row = {
                        "record_type": "job", "recorded_at_utc": utc_now(),
                        "elapsed_seconds": round(time.monotonic() - origin, 3),
                        "cycle_index": cycle_index + 1, "application": application,
                        "schedule_lag_seconds": round(lag, 3),
                        "submit_latency_ms": measurement["submit"],
                        "terminal_latency_ms": measurement["terminal"],
                        "result_latency_ms": measurement["result"],
                        "download_latency_ms": measurement["download"],
                        "total_latency_ms": measurement["total"], "final_state": "succeeded",
                        "artifact_verified": True, "server_pid_same": True,
                        "external_child_count": len(children),
                    }
                    job_records.append(row)
                    writer.append(row)
                except BaselineError as exc:
                    cycle_ok = False
                    cycle_failure = exc.category
                    failure_category = exc.category
                    failure_stage = "job"
                    failure_cycle = cycle_index + 1
                    break
            if cycle_ok:
                try:
                    cycle_health = _health(host, port)
                    if not cycle_health:
                        raise BaselineError("health_failure", "cycle health contract failed")
                except BaselineError as exc:
                    cycle_ok = False
                    cycle_failure = exc.category
                    failure_category = exc.category
                    failure_stage = "health"
                    failure_cycle = cycle_index + 1
            cycle_row = {
                "record_type": "cycle", "recorded_at_utc": utc_now(),
                "elapsed_seconds": round(time.monotonic() - origin, 3),
                "cycle_index": cycle_index + 1, "schedule_lag_seconds": round(lag, 3),
                "health_ok": cycle_health, "server_pid_same": process.poll() is None,
                "failure_category": cycle_failure,
                "total_latency_ms": round((time.monotonic() - cycle_started) * 1000.0, 3),
            }
            cycle_records.append(cycle_row)
            writer.append(cycle_row)
            if cycle_ok:
                _append_event(log_lines, "cycle_completed", cycle=cycle_index + 1)
            else:
                _append_event(log_lines, "cycle_failed", cycle=cycle_index + 1, category=cycle_failure)
                break
        if failure_category is None:
            while time.monotonic() < origin + duration_seconds:
                time.sleep(min(1.0, max(0.01, origin + duration_seconds - time.monotonic())))
    except KeyboardInterrupt:
        stop_requested = True
        failure_category = "interrupted"
        failure_stage = "run"
    except BaselineError as exc:
        failure_category = exc.category
        failure_stage = failure_stage or "setup"
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError):
        failure_category = failure_category or "internal_failure"
        failure_stage = failure_stage or "run"
    finally:
        if sampler is not None:
            sample_status = sampler.stop()
            if not sample_status["finished"] or sample_status["error_category"] is not None:
                failure_category = failure_category or sample_status["error_category"] or "resource_sampling_failure"
                failure_stage = failure_stage or "resource_sampling"
        if process is not None:
            cleanup["process_reaped"] = _terminate_process(process)
            cleanup["process_group_reaped"] = _process_group_reaped(process)
        for capture in captures:
            cleanup["capture_threads_finished"] = capture.join() and cleanup["capture_threads_finished"]
            if capture.overflow:
                failure_category = failure_category or "server_output_overflow"
                failure_stage = failure_stage or "cleanup"
        if writer is not None:
            cleanup["samples_closed"] = writer.close()
        try:
            config_path.unlink(missing_ok=True)
            cleanup["temporary_config_removed"] = not config_path.exists()
        except OSError:
            cleanup["temporary_config_removed"] = False
        try:
            shutil.rmtree(work_root)
            cleanup["work_root_removed"] = not work_root.exists()
            cleanup["runtime_copy_removed"] = cleanup["work_root_removed"]
        except OSError:
            cleanup["work_root_removed"] = False
            cleanup["runtime_copy_removed"] = False
        try:
            cleanup["port_released"] = port_is_available(host, port)
        except Exception:
            cleanup["port_released"] = False
        try:
            cleanup["result_tmp_files_removed"] = not any(run_dir.glob("*.tmp"))
        except OSError:
            cleanup["result_tmp_files_removed"] = False
        if failure_category is None and not all(cleanup.values()):
            failure_category = "cleanup_failure"
            failure_stage = "cleanup"

    finished_at = utc_now()
    elapsed_total = max(0.0, time.monotonic() - origin)
    outcome = "success" if failure_category is None and len(cycle_records) == expected_cycles and all(row.get("health_ok") for row in cycle_records) else "failure"
    if process_pid is not None and any(not row.get("server_pid_same", False) for row in cycle_records):
        outcome = "failure"
        failure_category = failure_category or "server_pid_changed"
    resource_result: dict[str, Any] | None = None
    if resource_samples:
        try:
            resource_result = _resource_summary(resource_samples, elapsed_total)
        except BaselineError as exc:
            failure_category = failure_category or exc.category
            failure_stage = failure_stage or "resource_summary"
            outcome = "failure"
    summary = {
        "schema_version": SCHEMA_VERSION, "run_id": run_id, "scenario": SCENARIO,
        "started_at_utc": started_at, "finished_at_utc": finished_at, "outcome": outcome,
        "failure_category": failure_category, "failure_stage": failure_stage,
        "failure_cycle": failure_cycle, "failure_application": failure_application,
        "last_successful_cycle": max((int(item["cycle_index"]) for item in cycle_records if item.get("failure_category") is None), default=0),
        "cycles_completed": len(cycle_records), "expected_cycles": expected_cycles,
        "jobs_expected": expected_cycles * 2, "jobs_completed": len(job_records),
        "ptv2": _job_latency_summary(job_records, "weld_inspection", duration_seconds),
        "weldagent": _job_latency_summary(job_records, "welding_guidance", duration_seconds),
        "health_checks": {"cycle_count": len(cycle_records), "failed": sum(not bool(row.get("health_ok")) for row in cycle_records)},
        "resource_trend": resource_result,
        "server_pid": process_pid, "server_pid_unchanged": all(row.get("server_pid_same", False) for row in cycle_records),
        "cleanup": cleanup,
        "method": {
            "duration_seconds": duration_seconds, "cadence_seconds": cadence_seconds,
            "cycles": expected_cycles, "jobs_per_cycle": 2, "serial": True,
            "applications_independent": True, "resource_interval_seconds": RESOURCE_INTERVAL_SECONDS,
            "resource_scope": "IAISF server PID only; PTV2/WeldAgent child processes and GPU excluded",
            "drain_scope": "single Application worker including LocalProcessRunner, adapter file handling, repository transitions, and HTTP polling",
            "not_queue_only_throughput": True,
            "not_application_acceptance_capacity": True,
            "not_concurrent_load": True, "not_ai_performance": True,
            "schedule_clock": "monotonic", "p95_p99_note": "sample count is recorded; no throughput claim",
        },
    }
    manifest = {
        "schema_version": SCHEMA_VERSION, "run_id": run_id, "scenario": SCENARIO,
        "started_at_utc": started_at, "finished_at_utc": finished_at,
        "git_sha": git["sha"], "git_dirty": git["dirty"], "build_configuration": "Release",
        "environment": environment_snapshot(), "method": summary["method"],
        "parameters": {"host": host, "duration_seconds": duration_seconds, "cadence_seconds": cadence_seconds,
                       "expected_cycles": expected_cycles, "jobs_per_cycle": 2,
                       "load_model": "serial PTV2 then serial WeldAgent; reuse uploaded ArtifactRef",
                       "external_process_resources_excluded": True, "not_ai_performance": True},
    }
    if writer is not None and writer.error is not None:
        summary["failure_category"] = summary["failure_category"] or writer.error
        summary["outcome"] = "failure"
    atomic_write_json(run_dir / "manifest.json", manifest)
    atomic_write_json(run_dir / "summary.json", summary)
    log_lines.extend([
        f"outcome={summary['outcome']}",
        f"cycles_completed={summary['cycles_completed']}",
        f"failure_category={summary['failure_category'] or 'none'}",
    ])
    atomic_write_bytes(run_dir / "run.log", ("\n".join(line[:200] for line in log_lines) + "\n").encode("utf-8"))
    return run_dir, summary


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="build/linux-release/iaisf_server")
    parser.add_argument("--config-template", default="build/mvp3-e2e.json")
    parser.add_argument("--ptv2-input", default="build/mvp3-e2e-input/inputs/http-ptv2/pointcloud.xyzf32le")
    parser.add_argument("--weldagent-input", default="build/mvp3-e2e-input/inputs/http-weldagent/pointcloud.xyzf32le")
    parser.add_argument("--weldagent-runtime", default="build/phase10d-e2e/weld-agent-runtime")
    parser.add_argument("--weldagent-tool-config", default="/mnt/e/weld_agent/config/tool_paths.local.json")
    parser.add_argument("--output-root", default="benchmarks/results")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18490)
    parser.add_argument("--duration-seconds", type=int, default=600)
    parser.add_argument("--cadence-seconds", type=int, default=CADENCE_SECONDS)
    parser.add_argument("--allow-dirty", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        run_dir, summary = _run_soak(
            repo_root=Path.cwd(), server_path=Path(args.server), config_template=Path(args.config_template),
            ptv2_input=Path(args.ptv2_input), weldagent_input=Path(args.weldagent_input),
            weldagent_runtime=Path(args.weldagent_runtime), weldagent_tool_config=Path(args.weldagent_tool_config),
            output_root=Path(args.output_root), duration_seconds=args.duration_seconds,
            cadence_seconds=args.cadence_seconds, host=args.host, port=args.port, allow_dirty=args.allow_dirty,
        )
    except BaselineError as exc:
        print(json.dumps({"outcome": "failure", "failure_category": exc.category}, sort_keys=True))
        return 2
    print(json.dumps({"outcome": summary["outcome"], "run_directory": run_dir.name,
                      "failure_category": summary["failure_category"]}, sort_keys=True))
    return 0 if summary["outcome"] == "success" else 1


if __name__ == "__main__":
    raise SystemExit(main())
