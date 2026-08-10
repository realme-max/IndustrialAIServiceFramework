#!/usr/bin/env python3
"""Run the bounded local HTTP health and point-cloud upload benchmark."""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any, Mapping, Sequence

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
    from run_baseline_smoke import (
        _PipeCapture,
        _process_group_reaped,
        _terminate_process,
    )
except ImportError:  # pragma: no cover - supports ``python -m`` execution
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
    from .run_baseline_smoke import (
        _PipeCapture,
        _process_group_reaped,
        _terminate_process,
    )


SCENARIO = "http-load"
HEALTH_CONCURRENCIES = (1, 8, 32, 128)
UPLOAD_CONCURRENCIES = (1, 2, 4)
HEALTH_WARMUP_SECONDS = 5.0
HEALTH_DURATION_SECONDS = 30.0
UPLOAD_PAYLOAD_BYTES = 30 * 1024 * 1024
PAYLOAD_LINE_BYTES = 40
UPLOAD_POINT_COUNT = 786_432
UPLOAD_CANONICAL_SIZE_BYTES = UPLOAD_POINT_COUNT * 12
UPLOAD_MIN_SUCCESSES = 20
UPLOAD_MAX_DURATION_SECONDS = 600.0
SAMPLER_INTERVAL_SECONDS = 0.1
HTTP_MAX_BODY_BYTES = 64 * 1024 * 1024
TCP_MAX_BUFFER_BYTES = 64 * 1024 * 1024
HTTP_HEADER_BYTES = 32 * 1024
MAX_RESPONSE_BYTES = 1 << 20
ARTIFACT_ID = re.compile(r"^pc_[0-9a-f]{64}$")
CSV_FIELDS = (
    "scenario",
    "concurrency",
    "duration_seconds",
    "requests",
    "succeeded",
    "failed",
    "uploaded_bytes",
    "throughput_requests_per_second",
    "throughput_mib_per_second",
    "latency_p50_ms",
    "latency_p95_ms",
    "latency_p99_ms",
    "latency_max_ms",
    "server_cpu_average_percent",
    "server_cpu_peak_percent",
    "server_rss_initial_bytes",
    "server_rss_peak_bytes",
    "client_cpu_seconds",
    "post_load_health_ok",
    "resource_sample_count",
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
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return round(ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower), 3)


def summarize_samples(
    scenario: str,
    concurrency: int,
    duration_seconds: float,
    requests: int,
    succeeded: int,
    failed: int,
    latencies_ms: Sequence[float],
    payload_bytes: int,
    server_cpu_average_percent: float | None,
    server_cpu_peak_percent: float | None,
    server_rss_initial_bytes: int | None,
    server_rss_peak_bytes: int | None,
    client_cpu_seconds: float,
    post_load_health_ok: bool | None = None,
    resource_sample_count: int | None = None,
) -> dict[str, Any]:
    if requests != succeeded + failed or requests < 0 or succeeded < 0 or failed < 0:
        raise BaselineError("invalid_measurement", "request counts are inconsistent")
    if duration_seconds <= 0 or not math.isfinite(duration_seconds):
        raise BaselineError("invalid_measurement", "duration is invalid")
    if any(not math.isfinite(float(value)) or value < 0 for value in latencies_ms):
        raise BaselineError("invalid_measurement", "latency is invalid")
    throughput = succeeded / duration_seconds
    mib = succeeded * payload_bytes / (1024 * 1024) / duration_seconds
    return {
        "scenario": scenario,
        "concurrency": concurrency,
        "duration_seconds": round(duration_seconds, 3),
        "requests": requests,
        "succeeded": succeeded,
        "failed": failed,
        "uploaded_bytes": succeeded * payload_bytes,
        "error_rate": _finite(round(failed / requests, 6) if requests else None),
        "throughput_requests_per_second": _finite(round(throughput, 3)),
        "throughput_mib_per_second": _finite(round(mib, 3)),
        "latency_p50_ms": _finite(_percentile(latencies_ms, 0.50)),
        "latency_p95_ms": _finite(_percentile(latencies_ms, 0.95)),
        "latency_p99_ms": _finite(_percentile(latencies_ms, 0.99)),
        "latency_max_ms": _finite(round(max(latencies_ms), 3) if latencies_ms else None),
        "server_cpu_average_percent": _finite(server_cpu_average_percent),
        "server_cpu_peak_percent": _finite(server_cpu_peak_percent),
        "server_rss_initial_bytes": server_rss_initial_bytes,
        "server_rss_peak_bytes": server_rss_peak_bytes,
        "client_cpu_seconds": _finite(round(client_cpu_seconds, 3)),
        "post_load_health_ok": post_load_health_ok,
        "resource_sample_count": resource_sample_count,
    }


def _validate_profiles(concurrencies: Sequence[int], allowed: Sequence[int]) -> tuple[int, ...]:
    values = tuple(concurrencies)
    if not values or any(value not in allowed for value in values) or len(set(values)) != len(values):
        raise BaselineError("invalid_profile", "concurrency profile is invalid")
    return values


def _make_xyz_payload(size_bytes: int, token: int) -> tuple[bytearray, int]:
    if size_bytes <= 0 or size_bytes % PAYLOAD_LINE_BYTES != 0:
        raise BaselineError("invalid_payload", "payload size must be a positive multiple of 40")
    line_count = size_bytes // PAYLOAD_LINE_BYTES
    if not 0 <= token <= 9_999_999_999:
        raise BaselineError("invalid_payload", "payload token is invalid")
    first_prefix = f"{token:010d}.0 0.0 0.0".encode("ascii")
    normal_prefix = b"0000000000.0 0.0 0.0"
    if len(first_prefix) != len(normal_prefix) or len(first_prefix) >= PAYLOAD_LINE_BYTES:
        raise BaselineError("invalid_payload", "payload line format is invalid")
    line = normal_prefix + b" " * (PAYLOAD_LINE_BYTES - len(normal_prefix) - 1) + b"\n"
    payload = bytearray(line * line_count)
    payload[: len(first_prefix)] = first_prefix
    return payload, line_count


def _health_contract(status: int, content_type: str, body: bytes) -> bool:
    if status != 200 or content_type.split(";", 1)[0].strip().lower() != "application/json":
        return False
    try:
        value = json.loads(body.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError):
        return False
    return (
        isinstance(value, dict)
        and value.get("status") == "ok"
        and value.get("live") is True
        and value.get("phase") == "running"
    )


def _upload_contract(status: int, content_type: str, body: bytes, expected_size: int, expected_points: int) -> str:
    if status != 201 or content_type.split(";", 1)[0].strip().lower() != "application/json":
        raise BaselineError("upload_contract", "upload response contract failed")
    if len(body) > MAX_RESPONSE_BYTES:
        raise BaselineError("upload_response_too_large", "upload response exceeds limit")
    try:
        value = json.loads(body.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("upload_contract", "upload response contract failed") from exc
    if not isinstance(value, dict) or set(value) != {"schema_version", "artifact", "download_url"}:
        raise BaselineError("upload_contract", "upload response contract failed")
    artifact = value.get("artifact")
    required = {
        "artifact_id", "sha256", "size_bytes", "kind", "media_type",
        "coordinate_frame", "unit", "point_count",
    }
    if not isinstance(artifact, dict) or set(artifact) != required:
        raise BaselineError("upload_contract", "artifact contract failed")
    artifact_id = artifact.get("artifact_id")
    if (
        not isinstance(artifact_id, str) or not ARTIFACT_ID.fullmatch(artifact_id)
        or not isinstance(artifact.get("sha256"), str)
        or not re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"])
        or artifact.get("size_bytes") != expected_size
        or artifact.get("point_count") != expected_points
        or artifact.get("kind") != "point_cloud"
        or artifact.get("media_type") != "application/vnd.iaisf.pointcloud.xyz-f32le"
        or artifact.get("coordinate_frame") != "camera"
        or artifact.get("unit") != "mm"
        or value.get("schema_version") != "1.0"
        or value.get("download_url") != f"/api/artifacts/v1/files/{artifact_id}"
    ):
        raise BaselineError("upload_contract", "artifact contract failed")
    return artifact_id


def _read_process_usage(pid: int) -> tuple[float, int] | None:
    if os.name != "posix":
        return None
    try:
        stat = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
        end_comm = stat.rfind(")")
        fields = stat[end_comm + 2 :].split()
        ticks = int(fields[11]) + int(fields[12])
        rss_pages = int(Path(f"/proc/{pid}/statm").read_text(encoding="ascii").split()[1])
        hz = os.sysconf("SC_CLK_TCK")
        page_size = os.sysconf("SC_PAGE_SIZE")
        return ticks / float(hz), rss_pages * page_size
    except (OSError, ValueError, IndexError, AttributeError):
        return None


class _ProcessSampler:
    def __init__(
        self,
        pid: int,
        initial_usage: tuple[float, int] | None,
        *,
        clock: Any = time.monotonic,
        wait: Any | None = None,
    ) -> None:
        self.pid = pid
        self._initial_usage = initial_usage
        self._clock = clock
        self._stop = threading.Event()
        self._wait = wait or self._stop.wait
        self._thread = threading.Thread(target=self._sample, name="iaisf-http-profile")
        self._samples: list[tuple[float, float, int]] = []
        self._lock = threading.Lock()

    def start(self) -> None:
        self._thread.start()

    def _sample(self) -> None:
        previous_cpu = self._initial_usage[0] if self._initial_usage is not None else None
        previous_time = self._clock()
        while True:
            if self._wait(SAMPLER_INTERVAL_SECONDS):
                break
            now = self._clock()
            usage = _read_process_usage(self.pid)
            if usage is None:
                previous_cpu = None
                previous_time = now
                continue
            cpu, rss = usage
            wall_delta = now - previous_time
            if previous_cpu is not None and wall_delta >= SAMPLER_INTERVAL_SECONDS and cpu >= previous_cpu:
                with self._lock:
                    self._samples.append((cpu - previous_cpu, wall_delta, rss))
            previous_cpu = cpu
            previous_time = now

    def stop(self) -> dict[str, Any]:
        self._stop.set()
        self._thread.join(timeout=5.0)
        with self._lock:
            samples = list(self._samples)
        valid = len(samples) >= 2
        if not valid:
            return {
                "finished": not self._thread.is_alive(),
                "valid": False,
                "sample_count": len(samples),
                "cpu_average_percent": None,
                "cpu_peak_percent": None,
                "rss_initial_bytes": self._initial_usage[1] if self._initial_usage is not None else None,
                "rss_peak_bytes": None,
            }
        cpu_seconds = sum(item[0] for item in samples)
        wall_seconds = sum(item[1] for item in samples)
        if wall_seconds <= 0 or not math.isfinite(wall_seconds):
            return {
                "finished": not self._thread.is_alive(),
                "valid": False,
                "sample_count": len(samples),
                "cpu_average_percent": None,
                "cpu_peak_percent": None,
                "rss_initial_bytes": self._initial_usage[1] if self._initial_usage is not None else None,
                "rss_peak_bytes": None,
            }
        cpu_percentages = [item[0] / item[1] * 100.0 for item in samples if item[1] > 0]
        if len(cpu_percentages) != len(samples):
            return {
                "finished": not self._thread.is_alive(),
                "valid": False,
                "sample_count": len(samples),
                "cpu_average_percent": None,
                "cpu_peak_percent": None,
                "rss_initial_bytes": self._initial_usage[1] if self._initial_usage is not None else None,
                "rss_peak_bytes": None,
            }
        rss_values = [item[2] for item in samples]
        initial_rss = self._initial_usage[1] if self._initial_usage is not None else None
        rss_peak = max(([initial_rss] if initial_rss is not None else []) + rss_values)
        return {
            "finished": not self._thread.is_alive(),
            "valid": True,
            "sample_count": len(samples),
            "cpu_average_percent": round(cpu_seconds / wall_seconds * 100.0, 3),
            "cpu_peak_percent": round(max(cpu_percentages), 3),
            "rss_initial_bytes": initial_rss,
            "rss_peak_bytes": rss_peak,
        }


def _request_health(connection: http.client.HTTPConnection, timeout: float) -> tuple[bool, float]:
    started = time.monotonic_ns()
    connection.request("GET", "/health", headers={"Connection": "keep-alive"})
    response = connection.getresponse()
    body = response.read(1 << 20)
    elapsed = (time.monotonic_ns() - started) / 1_000_000
    return _health_contract(response.status, response.headers.get("Content-Type", ""), body), elapsed


def _request_upload(
    connection: http.client.HTTPConnection,
    body: bytearray,
    timeout: float,
) -> tuple[bool, float, str | None]:
    started = time.monotonic_ns()
    connection.request(
        "POST",
        "/api/artifacts/v1/pointclouds",
        body=body,
        headers={
            "Content-Type": "text/plain; charset=utf-8",
            "Content-Length": str(len(body)),
            "Connection": "keep-alive",
        },
    )
    response = connection.getresponse()
    payload = response.read(MAX_RESPONSE_BYTES + 1)
    elapsed = (time.monotonic_ns() - started) / 1_000_000
    point_count = len(body) // PAYLOAD_LINE_BYTES
    canonical_size = point_count * 12
    artifact_id = _upload_contract(response.status, response.headers.get("Content-Type", ""), payload,
                                   canonical_size, point_count)
    return True, elapsed, artifact_id


def _require_post_load_health(host: str, port: int, timeout_seconds: float) -> bool:
    probe = probe_http(host, port, "/health", timeout_seconds)
    if not probe.get("ok"):
        raise BaselineError("post_load_health_failure", "post-load health contract failed")
    return True


def _stop_sampler_then_probe(
    sampler: _ProcessSampler,
    host: str,
    port: int,
    timeout_seconds: float,
) -> tuple[dict[str, Any], bool]:
    usage = sampler.stop()
    return usage, _require_post_load_health(host, port, timeout_seconds)


def _run_workers(
    scenario: str,
    host: str,
    port: int,
    concurrency: int,
    timeout_seconds: float,
    duration_seconds: float,
    payload_bytes: int,
    target_successes: int | None,
    token_counter: list[int],
    token_lock: threading.Lock,
    seen_artifacts: set[str],
    artifact_lock: threading.Lock,
) -> tuple[int, int, list[float], float, list[str], int]:
    deadline = time.monotonic() + duration_seconds
    barrier = threading.Barrier(concurrency)
    results: list[tuple[int, int, list[float], float, list[str], int]] = []
    result_lock = threading.Lock()
    successes = 0
    inflight = 0
    success_lock = threading.Lock()

    def worker() -> None:
        nonlocal successes, inflight
        requests = 0
        succeeded = 0
        failed = 0
        latencies: list[float] = []
        errors: list[str] = []
        started_cpu = time.process_time()
        local_tokens = 0
        connection: http.client.HTTPConnection | None = None
        try:
            barrier.wait(timeout=max(1.0, min(duration_seconds, 30.0)))
            while time.monotonic() < deadline:
                reserved = False
                if target_successes is not None:
                    with success_lock:
                        if successes + inflight >= target_successes:
                            break
                        inflight += 1
                        reserved = True
                requests += 1
                duplicate = False
                try:
                    if connection is None:
                        try:
                            connection = http.client.HTTPConnection(host, port, timeout=timeout_seconds)
                        except OSError:
                            failed += 1
                            errors.append("connection_failure")
                            continue
                    if scenario == "health":
                        ok, latency = _request_health(connection, timeout_seconds)
                        artifact_id = None
                    else:
                        with token_lock:
                            token = token_counter[0]
                            token_counter[0] += 1
                        body, _ = _make_xyz_payload(payload_bytes, token)
                        ok, latency, artifact_id = _request_upload(connection, body, timeout_seconds)
                        local_tokens += 1
                    latencies.append(latency)
                    if ok:
                        duplicate = False
                        if artifact_id is not None:
                            with artifact_lock:
                                if artifact_id in seen_artifacts:
                                    duplicate = True
                                else:
                                    seen_artifacts.add(artifact_id)
                        if duplicate:
                            ok = False
                            errors.append("duplicate_artifact_id")
                        else:
                            succeeded += 1
                            with success_lock:
                                successes += 1
                    if not ok:
                        failed += 1
                        if not duplicate:
                            errors.append("response_contract")
                except (BaselineError, http.client.HTTPException, OSError, TimeoutError, ValueError):
                    failed += 1
                    errors.append("request_failure")
                    if connection is not None:
                        connection.close()
                        connection = None
                finally:
                    if reserved:
                        with success_lock:
                            inflight -= 1
        finally:
            if connection is not None:
                connection.close()
            with result_lock:
                results.append((requests, succeeded, latencies, time.process_time() - started_cpu, errors, local_tokens))

    threads = [threading.Thread(target=worker, name=f"iaisf-http-worker-{index}") for index in range(concurrency)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    combined = (0, 0, [], 0.0, [], 0)
    for result in results:
        combined = (
            combined[0] + result[0], combined[1] + result[1], combined[2] + result[2],
            combined[3] + result[3], combined[4] + result[4], combined[5] + result[5],
        )
    return combined


def _effective_config(template_path: Path, host: str, port: int, applications_enabled: bool, work_root: Path) -> bytes:
    try:
        value = json.loads(template_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("config_invalid", "configuration template is invalid") from exc
    if not isinstance(value, dict):
        raise BaselineError("config_invalid", "configuration template is not an object")
    value.setdefault("server", {})["host"] = host
    value["server"]["port"] = port
    reactor = value["server"].setdefault("reactor", {})
    reactor["max_timers"] = max(4096, int(reactor.get("max_timers", 4096)))
    tcp = value["server"].setdefault("tcp", {})
    tcp["max_connections"] = min(128, int(tcp.get("max_connections", 128)))
    value["server"]["tcp"]["input_maximum_capacity_bytes"] = TCP_MAX_BUFFER_BYTES
    value["server"]["tcp"]["idle_timeout_ms"] = 300000
    value.setdefault("http", {})
    value["http"]["body_timeout_ms"] = 300000
    limits = value["http"].setdefault("limits", {})
    limits["max_header_bytes"] = HTTP_HEADER_BYTES
    limits["max_body_bytes"] = HTTP_MAX_BODY_BYTES - HTTP_HEADER_BYTES
    limits["max_response_body_bytes"] = 1 << 20
    limits["max_routes"] = max(256, int(limits.get("max_routes", 256)))
    value["applications"] = {"enabled": False}
    if applications_enabled:
        artifact_root = work_root / "artifacts"
        scratch_root = work_root / "scratch"
        output_root = work_root / "outputs"
        for directory in (artifact_root, scratch_root, output_root):
            directory.mkdir(parents=True, exist_ok=True)
        value["applications"] = {
            "enabled": True,
            "artifact_root": str(artifact_root),
            "scratch_root": str(scratch_root),
            "output_root": str(output_root),
            "repository_capacity": 128,
            "queue_capacity": 16,
            "ptv2": {
                "executable": str(work_root / "ptv2-placeholder"),
                "working_directory": str(work_root),
                "engine": str(work_root / "engine-placeholder"),
                "plugin": str(work_root / "plugin-placeholder"),
                "timeout_ms": 1000,
            },
            "weld_agent": {
                "python_executable": str(work_root / "python-placeholder"),
                "project_root": str(work_root),
                "orchestrator": str(work_root / "orchestrator-placeholder"),
                "tool_config": str(work_root / "tool-config-placeholder"),
                "timeout_ms": 1000,
            },
        }
    value.setdefault("diagnostics", {})["enabled"] = False
    value.setdefault("metrics", {})["enabled"] = False
    return json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8") + b"\n"


def _run_server_profile(
    *,
    repo_root: Path,
    server_path: Path,
    config_template: Path,
    output_run_dir: Path,
    host: str,
    port: int,
    scenario: str,
    concurrency: int,
    warmup_seconds: float,
    duration_seconds: float,
    request_timeout_seconds: float,
    payload_bytes: int,
    target_successes: int | None,
    server_command: Sequence[str] | None,
    token_counter: list[int],
    token_lock: threading.Lock,
) -> tuple[dict[str, Any], dict[str, Any]]:
    work_root = Path(tempfile.mkdtemp(prefix=".http-work-", dir=str(output_run_dir)))
    process: subprocess.Popen[bytes] | None = None
    captures: list[_PipeCapture] = []
    sampler: _ProcessSampler | None = None
    seen_artifacts: set[str] = set()
    artifact_lock = threading.Lock()
    started = utc_now()
    config_sha: str | None = None
    server_sha: str | None = None
    failure: str | None = None
    post_load_health_ok: bool | None = None
    cleanup = {
        "process_reaped": True,
        "process_group_reaped": True,
        "capture_threads_finished": True,
        "port_released": None,
        "temporary_config_removed": False,
        "artifact_root_removed": False,
    }
    stats: dict[str, Any] | None = None
    try:
        validate_loopback_host(host)
        validate_port(port)
        if not 0 < request_timeout_seconds <= 60:
            raise BaselineError("invalid_timeout", "request timeout is invalid")
        config_bytes = _effective_config(config_template, host, port, scenario == "upload", work_root)
        config_sha = hashlib.sha256(config_bytes).hexdigest()
        server_sha = sha256_file(server_path)
        if not port_is_available(host, port):
            raise BaselineError("port_in_use", "requested loopback port is unavailable")
        temporary_config = work_root / ".config.json"
        atomic_write_bytes(temporary_config, config_bytes)
        command = list(server_command) if server_command is not None else [str(server_path.resolve())]
        command.extend(["--serve", "--config", str(temporary_config)])
        process = subprocess.Popen(
            command,
            cwd=str(repo_root),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=False,
            start_new_session=(os.name == "posix"),
        )
        assert process.stdout is not None and process.stderr is not None
        captures = [_PipeCapture(process.stdout), _PipeCapture(process.stderr)]
        for capture in captures:
            capture.start()
        deadline = time.monotonic() + 30.0
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise BaselineError("process_exit", "server exited before becoming ready")
            health = probe_http(host, port, "/health", request_timeout_seconds)
            if health["ok"]:
                break
            if health["status"] is not None:
                raise BaselineError("health_contract", "health endpoint contract failed")
            time.sleep(0.05)
        else:
            raise BaselineError("startup_timeout", "server did not become ready in time")

        if warmup_seconds > 0:
            warmup = _run_workers(
                scenario="health", host=host, port=port, concurrency=concurrency,
                timeout_seconds=request_timeout_seconds, duration_seconds=warmup_seconds,
                payload_bytes=0, target_successes=None, token_counter=token_counter,
                token_lock=token_lock, seen_artifacts=seen_artifacts, artifact_lock=artifact_lock,
            )
            if warmup[4]:
                raise BaselineError("warmup_failure", "warmup request failed")
        initial_usage = _read_process_usage(process.pid)
        if initial_usage is None:
            raise BaselineError("resource_sampling_failure", "initial process resource sample unavailable")
        sampler = _ProcessSampler(process.pid, initial_usage)
        started_measure = time.monotonic()
        client_cpu_start = time.process_time()
        sampler.start()
        result = _run_workers(
            scenario=scenario, host=host, port=port, concurrency=concurrency,
            timeout_seconds=request_timeout_seconds, duration_seconds=duration_seconds,
            payload_bytes=payload_bytes, target_successes=target_successes,
            token_counter=token_counter, token_lock=token_lock,
            seen_artifacts=seen_artifacts, artifact_lock=artifact_lock,
        )
        client_cpu_seconds = max(0.0, time.process_time() - client_cpu_start)
        actual_duration = max(0.001, time.monotonic() - started_measure)
        if scenario == "upload" and result[1] < (target_successes or 0):
            raise BaselineError("upload_samples_insufficient", "minimum successful upload sample count was not reached")
        if sampler is None:
            raise BaselineError("resource_sampling_failure", "resource sampler was not started")
        usage, post_load_health_ok = _stop_sampler_then_probe(
            sampler, host, port, request_timeout_seconds,
        )
        sampler = None
        if not usage.get("finished", False):
            raise BaselineError("sampler_cleanup_failure", "resource sampler thread did not finish")
        if not post_load_health_ok:
            raise BaselineError("post_load_health_failure", "post-load health contract failed")
        if not usage.get("valid", False):
            raise BaselineError("resource_sampling_failure", "resource sampler produced too few valid samples")
        stats = summarize_samples(
            scenario, concurrency, actual_duration, result[0], result[1], result[0] - result[1], result[2],
            payload_bytes if scenario == "upload" else 0,
            usage.get("cpu_average_percent"), usage.get("cpu_peak_percent"),
            usage.get("rss_initial_bytes"), usage.get("rss_peak_bytes"), client_cpu_seconds,
            post_load_health_ok, usage.get("sample_count"),
        )
        if result[4]:
            failure = "request_failure"
        elif scenario == "upload" and len(seen_artifacts) != result[1]:
            failure = "artifact_identity_failure"
    except BaselineError as exc:
        failure = exc.category
    except (OSError, ValueError, RuntimeError) as exc:
        del exc
        failure = "internal_failure"
    finally:
        if sampler is not None:
            sampler_result = sampler.stop()
            if not sampler_result.get("finished", False):
                failure = "sampler_cleanup_failure"
        if process is not None:
            if not _terminate_process(process):
                failure = "process_cleanup_failure"
            cleanup["process_reaped"] = process.poll() is not None
            cleanup["process_group_reaped"] = _process_group_reaped(process)
            cleanup["port_released"] = port_is_available(host, port)
        capture_results = [capture.join() for capture in captures]
        cleanup["capture_threads_finished"] = all(capture_results)
        if captures and any(capture.overflow for capture in captures):
            failure = "process_output_limit"
        temporary_config = work_root / ".config.json"
        try:
            temporary_config.unlink(missing_ok=True)
            cleanup["temporary_config_removed"] = not temporary_config.exists()
        except OSError:
            cleanup["temporary_config_removed"] = False
        try:
            shutil.rmtree(work_root)
            cleanup["artifact_root_removed"] = not work_root.exists()
        except OSError:
            cleanup["artifact_root_removed"] = False
        if not all(value is True for value in cleanup.values() if value is not None):
            failure = "cleanup_failure"
        if process is not None:
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()
    row = stats or summarize_samples(
        scenario, concurrency, max(duration_seconds, 0.001), 0, 0, 0, [],
        payload_bytes if scenario == "upload" else 0, None, None, None, None, 0.0,
    )
    row["failure_category"] = failure
    row["post_load_health_ok"] = post_load_health_ok
    row["resource_sample_count"] = row.get("resource_sample_count") or 0
    row["started_at_utc"] = started
    row["effective_config_sha256"] = config_sha
    row["server_sha256"] = server_sha
    return row, cleanup


def run_http_benchmark(
    *,
    repo_root: Path,
    server_path: Path,
    config_template: Path,
    output_root: Path,
    host: str = "127.0.0.1",
    health_concurrencies: Sequence[int] = HEALTH_CONCURRENCIES,
    upload_concurrencies: Sequence[int] = UPLOAD_CONCURRENCIES,
    warmup_seconds: float = HEALTH_WARMUP_SECONDS,
    health_duration_seconds: float = HEALTH_DURATION_SECONDS,
    upload_max_duration_seconds: float = UPLOAD_MAX_DURATION_SECONDS,
    upload_min_successes: int = UPLOAD_MIN_SUCCESSES,
    request_timeout_seconds: float = 5.0,
    allow_dirty: bool = False,
    server_command: Sequence[str] | None = None,
) -> tuple[Path, dict[str, Any]]:
    validate_loopback_host(host)
    health_profile = _validate_profiles(health_concurrencies, HEALTH_CONCURRENCIES)
    upload_profile = _validate_profiles(upload_concurrencies, UPLOAD_CONCURRENCIES)
    if warmup_seconds < 0 or health_duration_seconds <= 0 or upload_max_duration_seconds <= 0:
        raise BaselineError("invalid_duration", "benchmark duration is invalid")
    if upload_min_successes <= 0 or not 0 < request_timeout_seconds <= 60:
        raise BaselineError("invalid_parameters", "benchmark parameters are invalid")
    if not server_path.is_file() or server_path.is_symlink():
        raise BaselineError("server_missing", "server executable is unavailable")
    git = git_snapshot(repo_root)
    if git["dirty"] and not allow_dirty:
        raise BaselineError("git_dirty", "working tree is dirty")
    run_id = make_run_id(git["sha"], SCENARIO)
    run_dir, actual_run_id = create_unique_run_directory(output_root, run_id)
    rows: list[dict[str, Any]] = []
    cleanup_rows: list[dict[str, Any]] = []
    log_lines = ["http benchmark run created", "scenario=http-load"]
    token_counter = [0]
    token_lock = threading.Lock()
    started = utc_now()
    try:
        for concurrency in health_profile:
            row, cleanup = _run_server_profile(
                repo_root=repo_root, server_path=server_path, config_template=config_template,
                output_run_dir=run_dir, host=host, port=18081 + concurrency,
                scenario="health", concurrency=concurrency, warmup_seconds=warmup_seconds,
                duration_seconds=health_duration_seconds, request_timeout_seconds=request_timeout_seconds,
                payload_bytes=0, target_successes=None, server_command=server_command,
                token_counter=token_counter, token_lock=token_lock,
            )
            rows.append(row)
            cleanup_rows.append(cleanup)
        for index, concurrency in enumerate(upload_profile):
            row, cleanup = _run_server_profile(
                repo_root=repo_root, server_path=server_path, config_template=config_template,
                output_run_dir=run_dir, host=host, port=18150 + index,
                scenario="upload", concurrency=concurrency, warmup_seconds=warmup_seconds,
                duration_seconds=upload_max_duration_seconds, request_timeout_seconds=request_timeout_seconds,
                payload_bytes=UPLOAD_PAYLOAD_BYTES, target_successes=upload_min_successes,
                server_command=server_command, token_counter=token_counter, token_lock=token_lock,
            )
            rows.append(row)
            cleanup_rows.append(cleanup)
        overall_failure = next((row["failure_category"] for row in rows if row.get("failure_category")), None)
        outcome = "success" if overall_failure is None else "failure"
        summary = {
            "schema_version": SCHEMA_VERSION,
            "run_id": actual_run_id,
            "scenario": SCENARIO,
            "started_at_utc": started,
            "finished_at_utc": utc_now(),
            "outcome": outcome,
            "failure_category": overall_failure,
            "profiles": rows,
            "method": {
                "load_generator": "Python standard-library http.client",
                "client_server_same_host": True,
                "health_connection_mode": "one keep-alive connection per worker",
                "baseline_scope": "harness-specific loopback baseline",
                "not_server_theoretical_max_qps": True,
                "sampler_interval_seconds": SAMPLER_INTERVAL_SECONDS,
                "cpu_percent_definition": "100% = one logical CPU core",
            },
            "cleanup": {
                "all_profiles_clean": all(
                    all(value is True for value in cleanup.values() if value is not None)
                    for cleanup in cleanup_rows
                ),
                "profile_count": len(rows),
            },
        }
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "run_id": actual_run_id,
            "scenario": SCENARIO,
            "started_at_utc": started,
            "finished_at_utc": summary["finished_at_utc"],
            "git_sha": git["sha"],
            "git_dirty": git["dirty"],
            "build_configuration": "Release",
            "server_sha256": next((row.get("server_sha256") for row in rows if row.get("server_sha256")), None),
            "environment": environment_snapshot(),
            "parameters": {
                "host": host,
                "health_concurrency": list(health_profile),
                "upload_concurrency": list(upload_profile),
                "health_warmup_seconds": warmup_seconds,
                "health_duration_seconds": health_duration_seconds,
                "upload_min_successes": upload_min_successes,
                "upload_max_duration_seconds": upload_max_duration_seconds,
                "upload_payload_bytes": UPLOAD_PAYLOAD_BYTES,
                "upload_payload_line_bytes": PAYLOAD_LINE_BYTES,
                "upload_point_count": UPLOAD_POINT_COUNT,
                "upload_canonical_size_bytes": UPLOAD_CANONICAL_SIZE_BYTES,
                "upload_payload_unique_per_request": True,
                "sampler_interval_seconds": SAMPLER_INTERVAL_SECONDS,
                "cpu_percent_definition": "100% = one logical CPU core",
                "load_generator": "Python standard-library http.client",
                "client_server_same_host": True,
                "health_connection_mode": "one keep-alive connection per worker",
                "baseline_scope": "harness-specific loopback baseline",
                "not_server_theoretical_max_qps": True,
                "http_max_body_bytes_effective": HTTP_MAX_BODY_BYTES - HTTP_HEADER_BYTES,
                "http_max_body_bytes_hard_bound": HTTP_MAX_BODY_BYTES,
                "tcp_input_maximum_capacity_bytes": TCP_MAX_BUFFER_BYTES,
            },
        }
        atomic_write_json(run_dir / "manifest.json", manifest)
        atomic_write_json(run_dir / "summary.json", summary)
        atomic_write_csv(run_dir / "profiles.csv", rows, CSV_FIELDS)
        atomic_write_bytes(
            run_dir / "run.log",
            ("\n".join(log_lines + [f"profiles={len(rows)}", f"outcome={outcome}"]) + "\n").encode("utf-8"),
        )
        return run_dir, summary
    except Exception:
        # The run directory itself is evidence; callers still receive a bounded
        # structured failure when a profile could not be completed.
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="build/linux-release/iaisf_server")
    parser.add_argument("--config-template", default="benchmarks/configs/baseline-smoke.json")
    parser.add_argument("--output-root", default="benchmarks/results")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--request-timeout-seconds", type=float, default=5.0)
    parser.add_argument("--health-warmup-seconds", type=float, default=HEALTH_WARMUP_SECONDS)
    parser.add_argument("--health-duration-seconds", type=float, default=HEALTH_DURATION_SECONDS)
    parser.add_argument("--upload-max-duration-seconds", type=float, default=UPLOAD_MAX_DURATION_SECONDS)
    parser.add_argument("--upload-min-successes", type=int, default=UPLOAD_MIN_SUCCESSES)
    parser.add_argument("--allow-dirty", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        run_dir, summary = run_http_benchmark(
            repo_root=Path.cwd(), server_path=Path(args.server), config_template=Path(args.config_template),
            output_root=Path(args.output_root), host=args.host,
            warmup_seconds=args.health_warmup_seconds, health_duration_seconds=args.health_duration_seconds,
            upload_max_duration_seconds=args.upload_max_duration_seconds,
            upload_min_successes=args.upload_min_successes, request_timeout_seconds=args.request_timeout_seconds,
            allow_dirty=args.allow_dirty,
        )
    except BaselineError as exc:
        print(f"benchmark failed: {exc.category}", file=sys.stderr)
        return 2
    print(json.dumps({"run_directory": run_dir.name, "outcome": summary["outcome"]}, sort_keys=True))
    return 0 if summary["outcome"] == "success" else 1


if __name__ == "__main__":
    raise SystemExit(main())
