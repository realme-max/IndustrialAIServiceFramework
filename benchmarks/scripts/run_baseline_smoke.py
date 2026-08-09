#!/usr/bin/env python3
"""Run one bounded, reproducible IAISF local baseline smoke.

The command starts only the server executable supplied by the caller, probes
three loopback endpoints, sends SIGTERM, and writes a self-contained result
directory.  It does not invoke a shell and never records command lines or raw
process output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
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
        validate_loopback_host,
        validate_port,
        utc_now,
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
        validate_loopback_host,
        validate_port,
        utc_now,
    )


SCENARIO = "baseline-smoke"
ENDPOINTS = ("/health", "/version", "/metrics")
CSV_FIELDS = (
    "endpoint",
    "started_at_utc",
    "status",
    "content_type",
    "response_bytes",
    "latency_ms",
    "ok",
    "error_category",
)


class _PipeCapture:
    def __init__(self, stream: Any, limit: int = 64 * 1024) -> None:
        self.stream = stream
        self.limit = limit
        self.bytes_seen = 0
        self.overflow = False
        self._thread = threading.Thread(target=self._drain, name="iaisf-baseline-pipe")

    def start(self) -> None:
        self._thread.start()

    def _drain(self) -> None:
        while True:
            try:
                chunk = self.stream.read(4096)
            except OSError:
                return
            if not chunk:
                return
            if self.bytes_seen <= self.limit:
                self.bytes_seen = min(self.limit + 1, self.bytes_seen + len(chunk))
            if len(chunk) > 0 and self.bytes_seen > self.limit:
                self.overflow = True

    def join(self) -> bool:
        self._thread.join(timeout=5.0)
        return not self._thread.is_alive()


def _terminate_process(process: subprocess.Popen[bytes], wait_seconds: float = 5.0) -> bool:
    if os.name != "posix":
        if process.poll() is not None:
            return True
        try:
            process.terminate()
            process.wait(timeout=wait_seconds)
        except (OSError, subprocess.TimeoutExpired):
            try:
                process.kill()
                process.wait(timeout=wait_seconds)
            except (OSError, subprocess.TimeoutExpired):
                return False
        return process.poll() is not None

    def group_exists() -> bool:
        try:
            os.killpg(process.pid, 0)
            return True
        except ProcessLookupError:
            return False
        except OSError:
            return True

    try:
        if group_exists():
            os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=wait_seconds)
    except subprocess.TimeoutExpired:
        pass
    except ProcessLookupError:
        pass
    except OSError:
        return False

    if group_exists():
        try:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=wait_seconds)
        except subprocess.TimeoutExpired:
            return False
        except ProcessLookupError:
            pass
        except OSError:
            return False
    return process.poll() is not None and not group_exists()


def _process_group_reaped(process: subprocess.Popen[bytes] | None) -> bool:
    if process is None or process.poll() is None:
        return False if process is not None else True
    if os.name != "posix":
        return True
    try:
        os.killpg(process.pid, 0)
    except ProcessLookupError:
        return True
    except OSError:
        return False
    return False


def _effective_config(template_path: Path, host: str, port: int) -> bytes:
    try:
        raw = template_path.read_bytes()
        if len(raw) > 1 << 20:
            raise BaselineError("config_too_large", "configuration template exceeds limit")
        value = json.loads(raw.decode("utf-8"))
    except BaselineError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BaselineError("config_invalid", "configuration template is invalid") from exc
    if not isinstance(value, dict):
        raise BaselineError("config_invalid", "configuration template is not an object")
    server = value.setdefault("server", {})
    if not isinstance(server, dict):
        raise BaselineError("config_invalid", "server configuration is invalid")
    server["host"] = host
    server["port"] = port
    value["applications"] = {"enabled": False}
    diagnostics = value.setdefault("diagnostics", {})
    if not isinstance(diagnostics, dict):
        raise BaselineError("config_invalid", "diagnostics configuration is invalid")
    diagnostics["enabled"] = False
    metrics = value.setdefault("metrics", {})
    if not isinstance(metrics, dict):
        raise BaselineError("config_invalid", "metrics configuration is invalid")
    metrics["enabled"] = True
    metrics["endpoint"] = "/metrics"
    try:
        return json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8") + b"\n"
    except (TypeError, ValueError) as exc:
        raise BaselineError("config_invalid", "configuration could not be serialized") from exc


def _write_log(run_dir: Path, lines: Sequence[str]) -> None:
    # run.log is deliberately a bounded event log, never captured stdout/stderr.
    data = ("\n".join(line[:200] for line in lines) + "\n").encode("utf-8")
    atomic_write_bytes(run_dir / "run.log", data)


def run_baseline_smoke(
    *,
    repo_root: Path,
    server_path: Path,
    config_template: Path,
    build_type: str,
    output_root: Path,
    host: str,
    port: int,
    startup_timeout_seconds: float,
    request_timeout_seconds: float,
    allow_dirty: bool,
    server_command: Sequence[str] | None = None,
) -> tuple[Path, dict[str, Any]]:
    if build_type not in {"Debug", "Release"}:
        raise BaselineError("invalid_build_type", "build type must be Debug or Release")
    validate_loopback_host(host)
    validate_port(port)
    if not 0 < startup_timeout_seconds <= 600:
        raise BaselineError("invalid_timeout", "startup timeout is outside the supported range")
    if not 0 < request_timeout_seconds <= 60:
        raise BaselineError("invalid_timeout", "request timeout is outside the supported range")

    started_at = utc_now()
    git = git_snapshot(repo_root)
    run_id = make_run_id(git["sha"], SCENARIO)
    run_dir, actual_run_id = create_unique_run_directory(output_root, run_id)
    log_lines = ["baseline run created", f"scenario={SCENARIO}"]
    requests: list[dict[str, Any]] = []
    process: subprocess.Popen[bytes] | None = None
    captures: list[_PipeCapture] = []
    temporary_config: Path | None = None
    outcome = "failure"
    failure_category: str | None = None
    process_exit_code: int | None = None
    server_sha: str | None = None
    config_sha: str | None = None
    port_released: bool | None = None
    temporary_config_removed = True
    process_reaped = process is None
    process_group_reaped = process is None
    capture_threads_finished = True
    result_temp_files_removed = True

    try:
        if git["dirty"] and not allow_dirty:
            raise BaselineError("git_dirty", "working tree is dirty")
        if not config_template.is_file() or config_template.is_symlink():
            raise BaselineError("config_missing", "configuration template is unavailable")
        if not server_path.is_file() or server_path.is_symlink():
            raise BaselineError("server_missing", "server executable is unavailable")
        if os.name == "posix" and not os.access(server_path, os.X_OK):
            raise BaselineError("server_missing", "server executable is unavailable")
        server_sha = sha256_file(server_path)
        config_bytes = _effective_config(config_template, host, port)
        config_sha = hashlib.sha256(config_bytes).hexdigest()
        if not port_is_available(host, port):
            raise BaselineError("port_in_use", "requested loopback port is unavailable")
        temporary_config = run_dir / ".config.json"
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
        deadline = time.monotonic() + startup_timeout_seconds
        health_ready = False
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise BaselineError("process_exit", "server exited before becoming ready")
            if any(capture.overflow for capture in captures):
                raise BaselineError("process_output_limit", "server output exceeded limit")
            health = probe_http(host, port, "/health", request_timeout_seconds)
            if health["ok"]:
                health_ready = True
                break
            if health["status"] is not None:
                raise BaselineError(
                    health["error_category"] or "health_contract",
                    "health endpoint contract failed",
                )
            time.sleep(0.05)
        if not health_ready:
            raise BaselineError("startup_timeout", "server did not become ready in time")
        for endpoint in ENDPOINTS:
            request = probe_http(host, port, endpoint, request_timeout_seconds)
            requests.append(request)
            if not request["ok"]:
                raise BaselineError(
                    request["error_category"] or "endpoint_failure",
                    "required endpoint contract failed",
                )
            if any(capture.overflow for capture in captures):
                raise BaselineError("process_output_limit", "server output exceeded limit")
        # A test substitute (or a server that exits immediately after its
        # final probe) may have completed between the probe and this point.
        # Reap that normal exit before sending a platform-specific terminate,
        # which avoids turning a clean Windows exit into a forced-kill code.
        try:
            process.wait(timeout=min(1.0, request_timeout_seconds))
        except subprocess.TimeoutExpired:
            if not _terminate_process(process):
                raise BaselineError("shutdown_timeout", "server did not stop in time")
        else:
            if not _terminate_process(process):
                raise BaselineError("shutdown_timeout", "server process group did not stop")
        process_exit_code = process.returncode
        if process_exit_code != 0:
            raise BaselineError("process_exit", "server returned a nonzero exit code")
        port_released = port_is_available(host, port)
        if not port_released:
            raise BaselineError("port_cleanup", "server port was not released")
        outcome = "success"
        log_lines.append("health/version/metrics probes succeeded")
        log_lines.append("SIGTERM shutdown completed")
    except BaselineError as exc:
        failure_category = exc.category
        log_lines.append(f"failure_category={exc.category}")
    except (OSError, ValueError, RuntimeError) as exc:
        del exc
        failure_category = "internal_failure"
        log_lines.append("failure_category=internal_failure")
    finally:
        if process is not None:
            _terminate_process(process)
            process_exit_code = process.returncode
            if port_released is None and process.poll() is not None:
                port_released = port_is_available(host, port)
            process_reaped = process.poll() is not None
            process_group_reaped = _process_group_reaped(process)
        capture_results = [capture.join() for capture in captures]
        capture_threads_finished = all(capture_results)
        if process is not None:
            if process.stdout is not None:
                process.stdout.close()
            if process.stderr is not None:
                process.stderr.close()
        if captures and any(capture.overflow for capture in captures):
            log_lines.append("process_output_limit=exceeded")
            if outcome == "success":
                outcome = "failure"
                failure_category = "process_output_limit"
        if temporary_config is not None:
            try:
                temporary_config.unlink(missing_ok=True)
                log_lines.append("temporary_config_cleaned=true")
            except OSError:
                log_lines.append("temporary_config_cleaned=false")
                temporary_config_removed = False
        result_temp_files_removed = not any(
            item.name.startswith(".") and item.name.endswith(".tmp")
            for item in run_dir.iterdir()
        )
        cleanup = {
            "temporary_config_removed": temporary_config_removed
            and (temporary_config is None or not temporary_config.exists()),
            "process_reaped": process_reaped,
            "process_group_reaped": process_group_reaped,
            "capture_threads_finished": capture_threads_finished,
            "port_released": port_released,
            "result_temp_files_removed": result_temp_files_removed,
        }
        cleanup_ok = all(
            value is True
            for value in cleanup.values()
            if value is not None
        )
        if not cleanup_ok:
            outcome = "failure"
            failure_category = "cleanup_failure"
            log_lines.append("cleanup_failure=true")
        finished_at = utc_now()
        environment = environment_snapshot()
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "run_id": actual_run_id,
            "scenario": SCENARIO,
            "started_at_utc": started_at,
            "finished_at_utc": finished_at,
            "git_sha": git["sha"],
            "git_dirty": git["dirty"],
            "build_configuration": build_type,
            "server_sha256": server_sha,
            "config_sha256": config_sha,
            "environment": environment,
            "parameters": {
                "host": host,
                "port": port,
                "startup_timeout_seconds": startup_timeout_seconds,
                "request_timeout_seconds": request_timeout_seconds,
                "applications_enabled": False,
                "metrics_endpoint": "/metrics",
            },
            "input": {"name": "none", "size_bytes": 0, "sha256": None},
            "outcome": outcome,
            "failure_category": failure_category,
        }
        summary = {
            "schema_version": SCHEMA_VERSION,
            "run_id": actual_run_id,
            "scenario": SCENARIO,
            "outcome": outcome,
            "failure_category": failure_category,
            "process_exit_code": process_exit_code,
            "requests": requests,
            "process_output": {
                "stdout_bytes": captures[0].bytes_seen if captures else 0,
                "stderr_bytes": captures[1].bytes_seen if len(captures) > 1 else 0,
                "bounded": True,
            },
            "cleanup": cleanup,
        }
        atomic_write_json(run_dir / "manifest.json", manifest)
        atomic_write_json(run_dir / "summary.json", summary)
        atomic_write_csv(run_dir / "samples.csv", requests, CSV_FIELDS)
        _write_log(run_dir, log_lines)
    return run_dir, summary


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="build/linux-release/iaisf_server")
    parser.add_argument("--config-template", default="benchmarks/configs/baseline-smoke.json")
    parser.add_argument("--build-type", choices=("Debug", "Release"), default="Release")
    parser.add_argument("--output-root", default="benchmarks/results")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18181)
    parser.add_argument("--startup-timeout-seconds", type=float, default=30.0)
    parser.add_argument("--request-timeout-seconds", type=float, default=5.0)
    parser.add_argument("--allow-dirty", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    repo_root = Path(__file__).resolve().parents[2]
    try:
        run_dir, summary = run_baseline_smoke(
            repo_root=repo_root,
            server_path=(repo_root / args.server).resolve()
            if not Path(args.server).is_absolute()
            else Path(args.server),
            config_template=(repo_root / args.config_template).resolve()
            if not Path(args.config_template).is_absolute()
            else Path(args.config_template),
            build_type=args.build_type,
            output_root=(repo_root / args.output_root).resolve()
            if not Path(args.output_root).is_absolute()
            else Path(args.output_root),
            host=args.host,
            port=args.port,
            startup_timeout_seconds=args.startup_timeout_seconds,
            request_timeout_seconds=args.request_timeout_seconds,
            allow_dirty=args.allow_dirty,
        )
    except BaselineError as exc:
        print(f"baseline smoke blocked: {exc.category}", file=sys.stderr)
        return 2
    print(json.dumps({"run_directory_name": run_dir.name, **summary}, sort_keys=True))
    return 0 if summary["outcome"] == "success" else 1


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
