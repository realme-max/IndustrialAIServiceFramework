#!/usr/bin/env python3
"""Measure serial real PTV2 and WeldAgent HTTP application latency.

This benchmark is intentionally single-job and sequential.  It records only
bounded aggregate measurements; external process/GPU CPU time is not sampled.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import math
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
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
        sha256_file,
        utc_now,
        validate_loopback_host,
        validate_port,
    )
    from .run_baseline_smoke import _PipeCapture, _process_group_reaped, _terminate_process
    from .run_http_benchmark import _ProcessSampler, _read_process_usage


SCENARIO = "real-ai-latency"
WARMUP_COUNT = 2
MEASURED_COUNT = 20
POLL_INTERVAL_SECONDS = 0.25
JOB_TIMEOUT_SECONDS = 900.0
START_TIMEOUT_SECONDS = 30.0
REQUEST_TIMEOUT_SECONDS = 30.0
MAX_HTTP_RESPONSE_BYTES = 16 * 1024 * 1024
SAMPLER_INTERVAL_SECONDS = 0.1
HTTP_BODY_BYTES = 60 * 1024 * 1024
TCP_INPUT_BYTES = 64 * 1024 * 1024
TCP_OUTPUT_BYTES = 32 * 1024 * 1024
ARTIFACT_ID_RE = re.compile(r"^pc_[0-9a-f]{64}$")
PUBLIC_ARTIFACT_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
JOB_ID_RE = re.compile(r"^(wi|wg)_[a-z0-9]{32}$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
ALLOWED_STATES = {
    "accepted", "queued", "dispatching", "running", "cancelling",
    "succeeded", "waiting_human", "failed", "cancelled", "timed_out",
    "worker_lost",
}
PTV2_RESULT_KEYS = {
    "schema_version", "job_id", "application", "state", "version",
    "output_artifacts", "weld_points", "prediction", "weld_point_count",
    "weld_ratio", "length_mm", "inference_time_ms", "total_time_ms",
    "quality_assessment",
}
WELD_AGENT_PUBLIC_KEYS = {
    "schema_version", "job_id", "application", "weld_type",
    "coordinate_frame", "unit", "start", "end", "x_axis", "y_axis",
    "z_axis", "confidence", "disposition", "waiting_reason",
    "robot_execution_allowed",
}
CSV_FIELDS = (
    "scenario", "application", "input_sha256", "input_size_bytes",
    "canonical_size_bytes", "point_count", "warmup_jobs", "measured_jobs",
    "upload_status", "upload_latency_ms", "success_count", "failure_count",
    "submit_latency_min_ms", "submit_latency_mean_ms", "submit_latency_p50_ms",
    "submit_latency_p95_ms", "submit_latency_p99_ms", "submit_latency_max_ms",
    "terminal_latency_min_ms", "terminal_latency_mean_ms", "terminal_latency_p50_ms",
    "terminal_latency_p95_ms", "terminal_latency_p99_ms", "terminal_latency_max_ms",
    "result_latency_min_ms", "result_latency_mean_ms", "result_latency_p50_ms",
    "result_latency_p95_ms", "result_latency_p99_ms", "result_latency_max_ms",
    "download_latency_min_ms", "download_latency_mean_ms", "download_latency_p50_ms",
    "download_latency_p95_ms", "download_latency_p99_ms", "download_latency_max_ms",
    "total_latency_min_ms", "total_latency_mean_ms", "total_latency_p50_ms",
    "total_latency_p95_ms", "total_latency_p99_ms", "total_latency_max_ms",
    "ptv2_inference_time_ms", "ptv2_total_time_ms",
    "server_cpu_average_percent", "server_cpu_peak_percent",
    "server_rss_initial_bytes", "server_rss_peak_bytes", "resource_sample_count",
    "pre_health_ok", "post_health_ok", "client_cpu_seconds", "error_category",
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
    if not values:
        return {"min": None, "mean": None, "p50": None, "p95": None, "p99": None, "max": None}
    if any(not math.isfinite(value) or value < 0.0 for value in values):
        raise BaselineError("invalid_measurement", "latency sample is invalid")
    return {
        "min": round(min(values), 3),
        "mean": round(sum(values) / len(values), 3),
        "p50": _percentile(values, 0.50),
        "p95": _percentile(values, 0.95),
        "p99": _percentile(values, 0.99),
        "max": round(max(values), 3),
    }


def _request(host: str, port: int, method: str, path: str,
             body: bytes | None = None, content_type: str | None = None,
             timeout: float = REQUEST_TIMEOUT_SECONDS) -> tuple[int, str, bytes, float]:
    started = time.monotonic_ns()
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    headers = {"Connection": "close"}
    if body is not None:
        headers["Content-Type"] = content_type or "application/json"
        headers["Content-Length"] = str(len(body))
    try:
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        payload = response.read(MAX_HTTP_RESPONSE_BYTES + 1)
        media = (response.headers.get("Content-Type", "").split(";", 1)[0].strip().lower())
        elapsed = (time.monotonic_ns() - started) / 1_000_000.0
        if len(payload) > MAX_HTTP_RESPONSE_BYTES:
            raise BaselineError("response_too_large", "HTTP response exceeds benchmark limit")
        return int(response.status), media, payload, elapsed
    except (OSError, TimeoutError, http.client.HTTPException) as exc:
        raise BaselineError("http_failure", "HTTP request failed") from exc
    finally:
        connection.close()


def _json_response(status: int, media: str, body: bytes) -> Mapping[str, Any]:
    if status != 200 or media != "application/json":
        raise BaselineError("http_contract", "HTTP JSON response contract failed")
    try:
        value = json.loads(body.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("json_contract", "HTTP JSON response is invalid") from exc
    if not isinstance(value, dict):
        raise BaselineError("json_contract", "HTTP JSON response is not an object")
    return value


def _health(host: str, port: int) -> bool:
    status, media, body, _ = _request(host, port, "GET", "/health")
    value = _json_response(status, media, body)
    return value.get("status") == "ok" and value.get("live") is True and value.get("phase") == "running"


def _sha256(path: Path) -> str:
    return sha256_file(path)


def _input_spec(path: Path, expected_sha: str, expected_size: int,
                point_count: int) -> dict[str, Any]:
    if not path.is_file() or path.is_symlink():
        raise BaselineError("input_missing", "required point-cloud input is unavailable")
    if path.stat().st_size != expected_size:
        raise BaselineError("input_size", "point-cloud input size is unexpected")
    digest = _sha256(path)
    if digest != expected_sha:
        raise BaselineError("input_sha256", "point-cloud input digest is unexpected")
    return {"path": path, "sha256": digest, "size_bytes": expected_size,
            "point_count": point_count, "canonical_size_bytes": point_count * 12}


def _write_xyz_text(source: Path, destination: Path, point_count: int) -> None:
    try:
        with source.open("rb") as input_stream, destination.open("xb") as output:
            for _ in range(point_count):
                chunk = input_stream.read(12)
                if len(chunk) != 12:
                    raise BaselineError("input_binary", "point-cloud binary is truncated")
                values = struct.unpack("<fff", chunk)
                if not all(math.isfinite(value) for value in values):
                    raise BaselineError("input_binary", "point-cloud coordinate is not finite")
                output.write((f"{values[0]:.9g} {values[1]:.9g} {values[2]:.9g}\n").encode("ascii"))
            if input_stream.read(1):
                raise BaselineError("input_binary", "point-cloud binary has extra bytes")
            output.flush()
            os.fsync(output.fileno())
    except BaselineError:
        destination.unlink(missing_ok=True)
        raise
    except (OSError, struct.error, UnicodeError, ValueError) as exc:
        destination.unlink(missing_ok=True)
        raise BaselineError("input_text_generation", "point-cloud text conversion failed") from exc


def _validate_artifact(value: Any, *, expected_size: int, expected_points: int) -> dict[str, Any]:
    required = {"artifact_id", "sha256", "size_bytes", "kind", "media_type",
                "coordinate_frame", "unit", "point_count"}
    if not isinstance(value, dict) or set(value) != required:
        raise BaselineError("artifact_contract", "artifact response contract failed")
    if (
        not isinstance(value["artifact_id"], str) or not ARTIFACT_ID_RE.fullmatch(value["artifact_id"])
        or not isinstance(value["sha256"], str) or not SHA_RE.fullmatch(value["sha256"])
        or value["size_bytes"] != expected_size or value["point_count"] != expected_points
        or value["kind"] != "point_cloud"
        or value["media_type"] != "application/vnd.iaisf.pointcloud.xyz-f32le"
        or value["coordinate_frame"] != "camera" or value["unit"] != "mm"
    ):
        raise BaselineError("artifact_contract", "artifact metadata is invalid")
    return dict(value)


def _upload(host: str, port: int, text_path: Path, expected_size: int,
            expected_points: int) -> tuple[dict[str, Any], float]:
    try:
        body = text_path.read_bytes()
    except OSError as exc:
        raise BaselineError("input_text_read", "point-cloud text cannot be read") from exc
    started = time.monotonic_ns()
    status, media, response, _ = _request(
        host, port, "POST", "/api/artifacts/v1/pointclouds", body,
        "text/plain; charset=utf-8", timeout=120.0)
    elapsed = (time.monotonic_ns() - started) / 1_000_000.0
    if status != 201 or media != "application/json":
        raise BaselineError("upload_contract", "point-cloud upload contract failed")
    try:
        value = json.loads(response.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("upload_contract", "point-cloud upload response is invalid") from exc
    if not isinstance(value, dict) or set(value) != {"schema_version", "artifact", "download_url"}:
        raise BaselineError("upload_contract", "point-cloud upload response is invalid")
    artifact = _validate_artifact(value.get("artifact"), expected_size=expected_size,
                                  expected_points=expected_points)
    if value.get("schema_version") != "1.0" or value.get("download_url") != "/api/artifacts/v1/files/" + artifact["artifact_id"]:
        raise BaselineError("upload_contract", "point-cloud upload URL is invalid")
    return artifact, round(elapsed, 3)


def _submit(host: str, port: int, application: str, artifact: Mapping[str, Any]) -> tuple[str, float]:
    inspection = application == "weld_inspection"
    path = "/api/weld-inspection/v1/jobs" if inspection else "/api/welding-guidance/v1/jobs"
    body: dict[str, Any] = {"schema_version": "1.0", "input_artifacts": [dict(artifact)]}
    if inspection:
        body["requested_outputs"] = ["segmentation", "geometry"]
        prefix = "/api/weld-inspection/v1/jobs/"
    else:
        body["weld_type"] = {"mode": "requested", "requested": "straight"}
        body["review_policy"] = {"human_checkpoint": "not_required"}
        prefix = "/api/welding-guidance/v1/jobs/"
    status, media, response, elapsed = _request(
        host, port, "POST", path, json.dumps(body, separators=(",", ":")).encode("utf-8"))
    if status != 202 or media != "application/json":
        raise BaselineError("submit_contract", "application submit contract failed")
    try:
        value = json.loads(response.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("submit_contract", "application submit response is invalid") from exc
    job_id = value.get("job_id") if isinstance(value, dict) else None
    status_url = value.get("status_url") if isinstance(value, dict) else None
    expected_id_prefix = "wi_" if inspection else "wg_"
    if (
        set(value) != {"job_id", "status_url"}
        or not isinstance(job_id, str) or not JOB_ID_RE.fullmatch(job_id)
        or not job_id.startswith(expected_id_prefix)
        or status_url != prefix + job_id
    ):
        raise BaselineError("submit_contract", "application submit identifiers are invalid")
    return job_id, round(elapsed, 3)


def _poll(host: str, port: int, application: str, job_id: str) -> tuple[str, float]:
    prefix = "/api/weld-inspection/v1/jobs/" if application == "weld_inspection" else "/api/welding-guidance/v1/jobs/"
    started = time.monotonic()
    deadline = started + JOB_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        status, media, response, _ = _request(host, port, "GET", prefix + job_id)
        value = _json_response(status, media, response)
        if value.get("job_id") != job_id or value.get("application") != application:
            raise BaselineError("status_contract", "application status identity is invalid")
        state = value.get("state")
        if not isinstance(state, str) or state not in ALLOWED_STATES:
            raise BaselineError("status_contract", "application status state is invalid")
        if state in {"succeeded", "waiting_human", "failed", "cancelled", "timed_out", "worker_lost"}:
            return state, round((time.monotonic() - started) * 1000.0, 3)
        time.sleep(POLL_INTERVAL_SECONDS)
    raise BaselineError("job_timeout", "application job did not reach a terminal state")


def _download(host: str, port: int, artifact: Mapping[str, Any]) -> tuple[float, bytes]:
    artifact_id = artifact.get("artifact_id")
    if not isinstance(artifact_id, str) or not PUBLIC_ARTIFACT_ID_RE.fullmatch(artifact_id):
        raise BaselineError("download_contract", "output artifact id is invalid")
    path = "/api/artifacts/v1/files/" + artifact_id
    if artifact.get("download_url") != path:
        raise BaselineError("download_contract", "output artifact download URL is invalid")
    status, media, body, elapsed = _request(host, port, "GET", path, timeout=60.0)
    if status != 200 or media != str(artifact.get("media_type")):
        raise BaselineError("download_contract", "output artifact response is invalid")
    if len(body) != artifact.get("size_bytes"):
        raise BaselineError("download_contract", "output artifact size is invalid")
    digest = hashlib.sha256(body).hexdigest()
    if digest != artifact.get("sha256"):
        raise BaselineError("download_contract", "output artifact digest is invalid")
    return round(elapsed, 3), body


def _result(host: str, port: int, application: str, job_id: str) -> tuple[Mapping[str, Any], float]:
    prefix = "/api/weld-inspection/v1/results/" if application == "weld_inspection" else "/api/welding-guidance/v1/results/"
    status, media, response, elapsed = _request(host, port, "GET", prefix + job_id)
    return _json_response(status, media, response), round(elapsed, 3)


def _verify_ptv2(value: Mapping[str, Any], point_count: int, job_id: str) -> list[Mapping[str, Any]]:
    if set(value) != PTV2_RESULT_KEYS or value.get("schema_version") != "1.0" or value.get("job_id") != job_id or value.get("state") != "succeeded" or value.get("application") != "weld_inspection":
        raise BaselineError("result_contract", "PTV2 result schema is invalid")
    if not isinstance(point_count, int) or point_count <= 0 or not isinstance(value.get("weld_point_count"), int):
        raise BaselineError("result_contract", "PTV2 result point count is invalid")
    weld_count = value["weld_point_count"]
    ratio = value.get("weld_ratio")
    if weld_count < 0 or weld_count > point_count or not isinstance(ratio, (int, float)) or isinstance(ratio, bool) or not math.isfinite(float(ratio)) or not 0.0 <= float(ratio) <= 1.0:
        raise BaselineError("result_contract", "PTV2 result metrics are invalid")
    for field in ("length_mm", "inference_time_ms", "total_time_ms"):
        if not isinstance(value.get(field), (int, float)) or isinstance(value.get(field), bool) or not math.isfinite(float(value[field])):
            raise BaselineError("result_contract", "PTV2 timing metric is invalid")
    if value.get("quality_assessment") != "not_implemented":
        raise BaselineError("result_contract", "PTV2 quality contract is invalid")
    outputs = value.get("output_artifacts")
    if not isinstance(outputs, list) or not outputs:
        raise BaselineError("result_contract", "PTV2 output artifacts are missing")
    unique: dict[str, Mapping[str, Any]] = {}
    for artifact in outputs:
        if not isinstance(artifact, dict):
            raise BaselineError("result_contract", "PTV2 output artifact is invalid")
        unique[str(artifact.get("artifact_id"))] = artifact
    if value.get("weld_points") is None or value.get("prediction") is None:
        raise BaselineError("result_contract", "PTV2 required outputs are missing")
    return list(unique.values())


def _verify_weldagent(value: Mapping[str, Any], job_id: str) -> list[Mapping[str, Any]]:
    if set(value) - (WELD_AGENT_PUBLIC_KEYS | {"output_artifacts", "state", "version"}) or value.get("schema_version") != "1.0" or value.get("job_id") != job_id or value.get("application") != "welding_guidance" or value.get("state") != "succeeded" or value.get("disposition") != "completed" or value.get("robot_execution_allowed") is not False or value.get("weld_type") != "straight" or value.get("coordinate_frame") != "camera" or value.get("unit") != "mm" or "corner" in value:
        raise BaselineError("result_contract", "WeldAgent result safety contract is invalid")
    for field in ("start", "end", "x_axis", "y_axis", "z_axis"):
        point = value.get(field)
        if not isinstance(point, list) or len(point) != 3 or any(not isinstance(item, (int, float)) or isinstance(item, bool) or not math.isfinite(float(item)) for item in point):
            raise BaselineError("result_contract", "WeldAgent geometry is invalid")
    confidence = value.get("confidence")
    if not isinstance(confidence, (int, float)) or isinstance(confidence, bool) or not math.isfinite(float(confidence)):
        raise BaselineError("result_contract", "WeldAgent confidence is invalid")
    outputs = value.get("output_artifacts")
    if not isinstance(outputs, list) or len(outputs) != 1 or not isinstance(outputs[0], dict):
        raise BaselineError("result_contract", "WeldAgent output artifact is invalid")
    return [outputs[0]]


def _run_one_job(host: str, port: int, application: str, artifact: Mapping[str, Any], point_count: int) -> dict[str, Any]:
    started = time.monotonic_ns()
    job_id, submit_latency = _submit(host, port, application, artifact)
    state, terminal_latency = _poll(host, port, application, job_id)
    if state != "succeeded":
        raise BaselineError("job_terminal_state", "real application job did not succeed")
    value, result_latency = _result(host, port, application, job_id)
    output_artifacts = _verify_ptv2(value, point_count, job_id) if application == "weld_inspection" else _verify_weldagent(value, job_id)
    download_started = time.monotonic_ns()
    downloaded_json: bytes | None = None
    for output in output_artifacts:
        _, body = _download(host, port, output)
        if application == "welding_guidance" and output.get("media_type") == "application/json":
            downloaded_json = body
    if application == "welding_guidance":
        if downloaded_json is None:
            raise BaselineError("result_contract", "WeldAgent public result download is missing")
        try:
            public = json.loads(downloaded_json.decode("utf-8"))
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise BaselineError("result_contract", "WeldAgent public result JSON is invalid") from exc
        if not isinstance(public, dict) or set(public) - WELD_AGENT_PUBLIC_KEYS or public.get("schema_version") != "1.0" or public.get("job_id") != job_id or public.get("robot_execution_allowed") is not False or public.get("application") != "welding_guidance" or public.get("weld_type") != "straight" or "corner" in public:
            raise BaselineError("result_contract", "WeldAgent public result is not allowlisted")
    download_latency = round((time.monotonic_ns() - download_started) / 1_000_000.0, 3)
    total_latency = round((time.monotonic_ns() - started) / 1_000_000.0, 3)
    measurement = {"submit": submit_latency, "terminal": terminal_latency, "result": result_latency,
                   "download": download_latency, "total": total_latency}
    if application == "weld_inspection":
        measurement["ptv2_inference_time_ms"] = float(value["inference_time_ms"])
        measurement["ptv2_total_time_ms"] = float(value["total_time_ms"])
    return measurement


def _row(application: str, spec: Mapping[str, Any], upload_latency: float,
         pre_health: bool, post_health: bool, measurements: Sequence[Mapping[str, Any]],
         usage: Mapping[str, Any], client_cpu: float, error: str | None = None) -> dict[str, Any]:
    row: dict[str, Any] = {"scenario": SCENARIO, "application": application,
                           "input_sha256": spec["sha256"], "input_size_bytes": spec["size_bytes"],
                           "canonical_size_bytes": spec["canonical_size_bytes"], "point_count": spec["point_count"],
                           "warmup_jobs": WARMUP_COUNT, "measured_jobs": MEASURED_COUNT,
                           "upload_status": "201", "upload_latency_ms": upload_latency,
                           "success_count": len(measurements), "failure_count": MEASURED_COUNT - len(measurements),
                           "pre_health_ok": pre_health, "post_health_ok": post_health,
                           "server_cpu_average_percent": usage.get("cpu_average_percent"),
                           "server_cpu_peak_percent": usage.get("cpu_peak_percent"),
                           "server_rss_initial_bytes": usage.get("rss_initial_bytes"),
                           "server_rss_peak_bytes": usage.get("rss_peak_bytes"),
                           "resource_sample_count": usage.get("sample_count"),
                           "client_cpu_seconds": round(max(0.0, client_cpu), 3), "error_category": error}
    for kind in ("submit", "terminal", "result", "download", "total"):
        stats = _latency_stats([float(item[kind]) for item in measurements])
        for name, value in stats.items():
            row[f"{kind}_latency_{name}_ms"] = _finite(value)
    if application == "weld_inspection":
        row["ptv2_inference_time_ms"] = _finite(
            round(sum(float(item["ptv2_inference_time_ms"]) for item in measurements) / len(measurements), 3)
        )
        row["ptv2_total_time_ms"] = _finite(
            round(sum(float(item["ptv2_total_time_ms"]) for item in measurements) / len(measurements), 3)
        )
    else:
        row["ptv2_inference_time_ms"] = None
        row["ptv2_total_time_ms"] = None
    return row


def _prepare_config(template: Path, host: str, port: int, work_root: Path,
                    runtime_root: Path) -> bytes:
    try:
        value = json.loads(template.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("config_invalid", "configuration template is invalid") from exc
    if not isinstance(value, dict):
        raise BaselineError("config_invalid", "configuration template is invalid")
    value.setdefault("server", {})["host"] = host
    value["server"]["port"] = port
    value["server"].setdefault("tcp", {})["input_maximum_capacity_bytes"] = TCP_INPUT_BYTES
    value["server"]["tcp"]["output_maximum_capacity_bytes"] = TCP_OUTPUT_BYTES
    limits = value.setdefault("http", {}).setdefault("limits", {})
    limits["max_body_bytes"] = HTTP_BODY_BYTES
    limits["max_response_body_bytes"] = 16 * 1024 * 1024
    applications = value.setdefault("applications", {})
    applications["enabled"] = True
    applications["artifact_root"] = str(work_root / "artifact-root")
    applications["scratch_root"] = str(work_root / "scratch-root")
    applications["output_root"] = str(work_root / "output-root")
    applications["repository_capacity"] = 128
    applications["queue_capacity"] = 8
    weld_agent = applications.setdefault("weld_agent", {})
    weld_agent["project_root"] = str(runtime_root)
    weld_agent["orchestrator"] = str(runtime_root / "agent_orchestrator.py")
    weld_agent["tool_config"] = str(runtime_root / "config" / "tool_paths.local.json")
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def _prepare_runtime(source: Path, external_tool_config: Path, destination: Path) -> None:
    if not source.is_dir() or source.is_symlink() or not external_tool_config.is_file() or external_tool_config.is_symlink():
        raise BaselineError("weldagent_runtime_missing", "WeldAgent runtime inputs are unavailable")
    try:
        for item in source.rglob("*"):
            if item.is_symlink():
                raise BaselineError("weldagent_runtime_invalid", "WeldAgent runtime contains a symlink")
        shutil.copytree(source, destination, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "tasks"))
        config_destination = destination / "config" / "tool_paths.local.json"
        config_destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(external_tool_config, config_destination)
        tool_value = json.loads(config_destination.read_text(encoding="utf-8-sig"))
        if not isinstance(tool_value, dict) or tool_value.get("allow_send_url") is not False:
            raise BaselineError("weldagent_safety_config", "WeldAgent send-url policy is not disabled")
    except BaselineError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("weldagent_runtime_copy", "WeldAgent runtime copy failed") from exc


def _cleanup_process(process: subprocess.Popen[bytes] | None, captures: Sequence[_PipeCapture]) -> dict[str, bool]:
    result = {"process_reaped": True, "process_group_reaped": True, "capture_threads_finished": True}
    if process is not None:
        result["process_reaped"] = _terminate_process(process)
        result["process_group_reaped"] = _process_group_reaped(process)
    for capture in captures:
        result["capture_threads_finished"] = capture.join() and result["capture_threads_finished"]
    return result


def run_real_ai_latency_benchmark(*, repo_root: Path, server_path: Path,
                                  config_template: Path, ptv2_input: Path,
                                  weldagent_input: Path, weldagent_runtime: Path,
                                  weldagent_tool_config: Path, output_root: Path,
                                  host: str = "127.0.0.1", port: int = 18480,
                                  allow_dirty: bool = False) -> tuple[Path, dict[str, Any]]:
    if os.name != "posix":
        raise BaselineError("platform_unsupported", "real AI benchmark requires Linux")
    validate_loopback_host(host)
    validate_port(port)
    git = git_snapshot(repo_root)
    if git["dirty"] and not allow_dirty:
        raise BaselineError("git_dirty", "working tree is dirty")
    if not server_path.is_file() or server_path.is_symlink():
        raise BaselineError("server_missing", "Linux Release server is unavailable")
    if not config_template.is_file() or config_template.is_symlink():
        raise BaselineError("config_missing", "benchmark configuration is unavailable")
    if not port_is_available(host, port):
        raise BaselineError("port_in_use", "benchmark port is unavailable")
    pt_spec = _input_spec(ptv2_input, "8c9bd45f520f4e85e914f1628ca2d366fb6983a917a0ea7818a3865e8ae8c8ea", 24576, 2048)
    wg_spec = _input_spec(weldagent_input, "40ea2c408eeb082559b706929161dba1d979f3e3634b24580e90fe94bc9806e7", 9877368, 823114)
    run_dir, run_id = create_unique_run_directory(output_root, make_run_id(git["sha"], SCENARIO))
    work_root = Path(tempfile.mkdtemp(prefix=".real-ai-", dir=str(output_root)))
    runtime_root = work_root / "weld-agent-runtime"
    config_path = work_root / "config.json"
    process: subprocess.Popen[bytes] | None = None
    captures: list[_PipeCapture] = []
    rows: list[dict[str, Any]] = []
    cleanup = {"process_reaped": True, "process_group_reaped": True, "capture_threads_finished": True,
               "temporary_config_removed": False, "runtime_copy_removed": False,
               "work_root_removed": False, "port_released": False}
    started = utc_now()
    failure: str | None = None
    try:
        _prepare_runtime(weldagent_runtime, weldagent_tool_config, runtime_root)
        for directory in (work_root / "artifact-root", work_root / "scratch-root", work_root / "output-root"):
            directory.mkdir(parents=True, exist_ok=False)
        config_bytes = _prepare_config(config_template, host, port, work_root, runtime_root)
        atomic_write_bytes(config_path, config_bytes)
        process = subprocess.Popen(
            [str(server_path.resolve()), "--serve", "--config", str(config_path.resolve())],
            cwd=str(repo_root), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, shell=False, start_new_session=True,
        )
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
        for application, spec in (("weld_inspection", pt_spec), ("welding_guidance", wg_spec)):
            pre_health = _health(host, port)
            if not pre_health:
                raise BaselineError("pre_load_health_failure", "pre-chain health contract failed")
            text_path = work_root / ("ptv2-input.txt" if application == "weld_inspection" else "weldagent-input.txt")
            _write_xyz_text(spec["path"], text_path, spec["point_count"])
            initial_usage = _read_process_usage(process.pid)
            if initial_usage is None:
                raise BaselineError("resource_sampling_failure", "initial server resource sample unavailable")
            sampler = _ProcessSampler(process.pid, initial_usage)
            sampler.start()
            client_cpu_started = time.process_time()
            try:
                artifact, upload_latency = _upload(host, port, text_path, spec["canonical_size_bytes"], spec["point_count"])
                for _ in range(WARMUP_COUNT):
                    _run_one_job(host, port, application, artifact, spec["point_count"])
                measurements = [_run_one_job(host, port, application, artifact, spec["point_count"]) for _ in range(MEASURED_COUNT)]
                usage = sampler.stop()
                sampler = None
                if not usage.get("finished") or not usage.get("valid"):
                    raise BaselineError("resource_sampling_failure", "resource sampler did not produce valid windows")
                post_health = _health(host, port)
                if not post_health:
                    raise BaselineError("post_load_health_failure", "post-chain health contract failed")
                rows.append(_row(application, spec, upload_latency, pre_health, post_health,
                                 measurements, usage, time.process_time() - client_cpu_started))
            finally:
                if sampler is not None:
                    sampler.stop()
        failure = None
    except BaselineError as exc:
        failure = exc.category
    except (OSError, ValueError, RuntimeError, json.JSONDecodeError):
        failure = "internal_failure"
    finally:
        process_cleanup = _cleanup_process(process, captures)
        cleanup.update(process_cleanup)
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
        if failure is None and not all(cleanup.values()):
            failure = "cleanup_failure"
    finished = utc_now()
    outcome = "success" if failure is None and len(rows) == 2 else "failure"
    summary = {
        "schema_version": SCHEMA_VERSION, "run_id": run_id, "scenario": SCENARIO,
        "started_at_utc": started, "finished_at_utc": finished, "outcome": outcome,
        "failure_category": failure, "profiles": rows, "cleanup": cleanup,
        "method": {
            "warmup_jobs_per_application": WARMUP_COUNT,
            "measured_jobs_per_application": MEASURED_COUNT,
            "poll_interval_seconds": POLL_INTERVAL_SECONDS,
            "serial_single_job": True,
            "resource_scope": "IAISF server PID only; external process and GPU excluded",
            "p95_p99_note": "20 measured samples; exploratory percentiles",
            "applications_independent": True,
        },
    }
    manifest = {
        "schema_version": SCHEMA_VERSION, "run_id": run_id, "scenario": SCENARIO,
        "started_at_utc": started, "finished_at_utc": finished,
        "git_sha": git["sha"], "git_dirty": git["dirty"], "build_configuration": "Release",
        "environment": environment_snapshot(),
        "method": summary["method"],
        "parameters": {"host": host, "warmup_jobs": WARMUP_COUNT, "measured_jobs": MEASURED_COUNT,
                       "poll_interval_seconds": POLL_INTERVAL_SECONDS,
                       "sampler_interval_seconds": SAMPLER_INTERVAL_SECONDS,
                       "load_generator": "Python standard-library http.client",
                       "client_server_same_host": True,
                       "real_ptv2_and_weldagent": True,
                       "not_ai_performance": True},
    }
    atomic_write_json(run_dir / "manifest.json", manifest)
    atomic_write_json(run_dir / "summary.json", summary)
    atomic_write_csv(run_dir / "profiles.csv", rows, CSV_FIELDS)
    atomic_write_bytes(run_dir / "run.log", ("real AI latency benchmark\nPTV2 and WeldAgent are independent\n" + f"outcome={outcome}\n").encode("utf-8"))
    return run_dir, summary


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="build/linux-release/iaisf_server")
    parser.add_argument("--config-template", default="build/mvp3-e2e.json")
    parser.add_argument("--ptv2-input", default="build/mvp3-e2e-input/inputs/http-ptv2/pointcloud.xyzf32le")
    parser.add_argument("--weldagent-input", default="build/mvp3-e2e-input/inputs/http-weldagent/pointcloud.xyzf32le")
    parser.add_argument("--weldagent-runtime", default="build/phase10d-e2e/weld-agent-runtime")
    parser.add_argument("--weldagent-tool-config", default="E:/weld_agent/config/tool_paths.local.json")
    parser.add_argument("--output-root", default="benchmarks/results")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18480)
    parser.add_argument("--allow-dirty", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        run_dir, summary = run_real_ai_latency_benchmark(
            repo_root=Path.cwd(), server_path=Path(args.server), config_template=Path(args.config_template),
            ptv2_input=Path(args.ptv2_input), weldagent_input=Path(args.weldagent_input),
            weldagent_runtime=Path(args.weldagent_runtime), weldagent_tool_config=Path(args.weldagent_tool_config),
            output_root=Path(args.output_root), host=args.host, port=args.port, allow_dirty=args.allow_dirty,
        )
    except BaselineError as exc:
        print(f"benchmark blocked: {exc.category}", file=sys.stderr)
        return 2
    print(json.dumps({"run_directory": run_dir.name, "outcome": summary["outcome"]}, sort_keys=True))
    return 0 if summary["outcome"] == "success" else 1


if __name__ == "__main__":
    raise SystemExit(main())
