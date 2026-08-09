"""Small, dependency-free helpers for reproducible IAISF baseline runs.

This module deliberately records facts and bounded request samples only.  It
never records a command line, environment, local path, or process output.
"""

from __future__ import annotations

import csv
import hashlib
import json
import os
import platform
import re
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


SCHEMA_VERSION = "1.0"
MAX_JSON_BYTES = 1 << 20
MAX_CAPTURE_BYTES = 64 << 10
MAX_RESPONSE_BYTES = 1 << 20
_SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
_SAFE_RUN_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
_SAFE_VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+() -]{0,127}$")
_PROMETHEUS_SAMPLE = re.compile(
    r"^[A-Za-z_:][A-Za-z0-9_:]*(?:\{[^{}\r\n]{0,512}\})?\s+"
    r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?(?:\s+\d+)?$"
)


class BaselineError(RuntimeError):
    """An expected, structured failure which is safe to expose in results."""

    def __init__(self, category: str, message: str) -> None:
        self.category = category
        # Callers pass short static messages.  Keep a final defensive bound so
        # an accidental exception text can never become an unbounded artifact.
        super().__init__(str(message)[:160])


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def make_run_id(git_sha: str, scenario: str) -> str:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    short_sha = re.sub(r"[^0-9a-f]", "", git_sha.lower())[:12] or "nogit"
    if not _SAFE_ID.fullmatch(scenario):
        raise BaselineError("invalid_scenario", "scenario is invalid")
    return f"{stamp}-{short_sha}-{scenario}"


def _validate_run_id(run_id: str) -> None:
    if not isinstance(run_id, str) or not _SAFE_RUN_ID.fullmatch(run_id):
        raise BaselineError("invalid_run_id", "run identifier is invalid")
    if run_id in {".", ".."}:
        raise BaselineError("invalid_run_id", "run identifier is invalid")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except (OSError, ValueError) as exc:
        raise BaselineError("file_read_failure", "unable to read required file") from exc
    return digest.hexdigest()


def _decode_bounded(data: bytes) -> str:
    return data[:MAX_CAPTURE_BYTES].decode("utf-8", errors="replace")


def run_command(
    argv: Sequence[str], *, timeout_seconds: float = 10.0, cwd: Path | None = None
) -> tuple[int | None, str, bool]:
    """Run an argv vector without a shell and return exit code, output, timeout."""

    try:
        completed = subprocess.run(
            list(argv),
            cwd=str(cwd) if cwd is not None else None,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            shell=False,
            timeout=timeout_seconds,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None, "unavailable", True
    return completed.returncode, _decode_bounded(completed.stdout), False


def git_snapshot(repo_root: Path) -> dict[str, Any]:
    code, output, timed_out = run_command(
        ["git", "rev-parse", "HEAD"], cwd=repo_root, timeout_seconds=5
    )
    if timed_out or code != 0:
        raise BaselineError("git_unavailable", "git commit could not be identified")
    git_sha = output.strip().splitlines()[0] if output.strip() else ""
    if not re.fullmatch(r"[0-9a-fA-F]{40}", git_sha):
        raise BaselineError("git_unavailable", "git commit could not be identified")
    code, output, timed_out = run_command(
        ["git", "status", "--porcelain=v1"], cwd=repo_root, timeout_seconds=5
    )
    if timed_out or code != 0:
        raise BaselineError("git_unavailable", "git status could not be read")
    return {"sha": git_sha.lower(), "dirty": bool(output.strip())}


def _memory_total_bytes() -> int | None:
    meminfo = Path("/proc/meminfo")
    try:
        for line in meminfo.read_text(encoding="ascii").splitlines():
            if line.startswith("MemTotal:"):
                value = int(line.split()[1])
                return value * 1024
    except (OSError, ValueError, IndexError):
        return None
    return None


def _cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="replace").splitlines():
            if line.lower().startswith("model name"):
                value = line.split(":", 1)[-1].strip()
                return re.sub(r"[^A-Za-z0-9 ._()+@-]", "?", value)[:128]
    except OSError:
        pass
    value = platform.processor() or platform.machine() or "unknown"
    return re.sub(r"[^A-Za-z0-9 ._()+@-]", "?", value)[:128]


def environment_snapshot() -> dict[str, Any]:
    compiler_code, compiler, compiler_timeout = run_command(["c++", "--version"])
    cmake_code, cmake, cmake_timeout = run_command(["cmake", "--version"])

    def safe_version(code: int | None, text: str, timed_out: bool) -> str:
        if timed_out or code != 0:
            return "unavailable"
        first = text.splitlines()[0].strip() if text.splitlines() else ""
        if not first or len(first) > 160 or any(ord(char) < 0x20 for char in first):
            return "unavailable"
        try:
            first.encode("ascii")
        except UnicodeEncodeError:
            return "unavailable"
        return first

    def safe_field(value: object, limit: int = 128) -> str:
        text = str(value).encode("ascii", errors="replace").decode("ascii")
        return re.sub(r"[^A-Za-z0-9 ._+()@:-]", "?", text)[:limit]

    os_name = "unknown"
    os_version = "unknown"
    release_file = Path("/etc/os-release")
    try:
        allowed = {"NAME", "VERSION", "VERSION_ID", "PRETTY_NAME"}
        values: dict[str, str] = {}
        for line in release_file.read_text(encoding="utf-8", errors="strict").splitlines():
            key, separator, value = line.partition("=")
            if separator and key in allowed:
                value = value.strip().strip('"')
                value = re.sub(r"[^A-Za-z0-9 ._+()@:-]", "?", value)[:128]
                values[key] = value
        os_name = values.get("PRETTY_NAME", values.get("NAME", os_name))
        os_version = values.get("VERSION_ID", values.get("VERSION", os_version))
    except (OSError, UnicodeError):
        os_name = platform.system() or os_name
        os_version = platform.release()[:128] or os_version

    return {
        "os_name": safe_field(os_name),
        "os_version": safe_field(os_version),
        "kernel": safe_field(platform.release()),
        "machine": safe_field(platform.machine(), 64),
        "cpu_model": _cpu_model(),
        "logical_cpu_count": os.cpu_count(),
        "memory_total_bytes": _memory_total_bytes(),
        "compiler_version": safe_version(compiler_code, compiler, compiler_timeout),
        "cmake_version": safe_version(cmake_code, cmake, cmake_timeout),
        "python_version": safe_field(platform.python_version(), 32),
    }


def atomic_write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        raise BaselineError("output_exists", "result file already exists")
    temporary: Path | None = None
    file_descriptor: int | None = None
    try:
        # mkstemp claims a distinct temporary inode even when several threads
        # in this process target the same destination concurrently.
        file_descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent)
        )
        temporary = Path(temporary_name)
        with os.fdopen(file_descriptor, "wb") as stream:
            file_descriptor = None
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        # A hard-link commit is atomic and refuses to replace a destination
        # created by a racing writer on both POSIX and supported Windows file
        # systems.  The temporary inode is then removed, leaving one stable
        # destination and no temporary artifact.
        try:
            os.link(temporary, path)
        except FileExistsError as exc:
            raise BaselineError("output_exists", "result file already exists") from exc
        temporary.unlink()
    except FileExistsError as exc:
        try:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise BaselineError("output_exists", "result file already exists") from exc
    except BaselineError:
        try:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    except (OSError, ValueError) as exc:
        try:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise BaselineError("output_write_failure", "unable to commit result file") from exc
    finally:
        if file_descriptor is not None:
            try:
                os.close(file_descriptor)
            except OSError:
                pass


def atomic_write_json(path: Path, value: Mapping[str, Any]) -> None:
    try:
        data = json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise BaselineError("serialization_failure", "unable to serialize result") from exc
    if len(data) > MAX_JSON_BYTES:
        raise BaselineError("result_too_large", "serialized result exceeds limit")
    atomic_write_bytes(path, data + b"\n")


def atomic_write_csv(path: Path, rows: Iterable[Mapping[str, Any]], fieldnames: Sequence[str]) -> None:
    try:
        with tempfile.SpooledTemporaryFile(max_size=MAX_JSON_BYTES, mode="w+", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="raise")
            writer.writeheader()
            for row in rows:
                normalized = {
                    field: ""
                    if row.get(field) is None
                    else "true"
                    if row.get(field) is True
                    else "false"
                    if row.get(field) is False
                    else row.get(field)
                    for field in fieldnames
                }
                writer.writerow(normalized)
            stream.seek(0)
            data = stream.read().encode("utf-8")
    except (OSError, ValueError, TypeError, UnicodeError) as exc:
        raise BaselineError("serialization_failure", "unable to serialize samples") from exc
    if len(data) > MAX_JSON_BYTES:
        raise BaselineError("result_too_large", "serialized samples exceed limit")
    atomic_write_bytes(path, data)


def create_unique_run_directory(output_root: Path, run_id: str) -> tuple[Path, str]:
    _validate_run_id(run_id)
    try:
        if output_root.is_symlink():
            raise BaselineError("invalid_output_root", "output root is not a directory")
        output_root.mkdir(parents=True, exist_ok=True)
        if not output_root.is_dir() or output_root.is_symlink():
            raise BaselineError("invalid_output_root", "output root is not a directory")
        canonical_root = output_root.resolve(strict=True)
        for suffix in [""] + [f"-{index:02d}" for index in range(1, 100)]:
            candidate_id = run_id + suffix
            _validate_run_id(candidate_id)
            candidate = output_root / candidate_id
            if candidate.parent.resolve(strict=True) != canonical_root:
                raise BaselineError("invalid_run_id", "run identifier escaped output root")
            try:
                candidate.mkdir()
                if candidate.resolve(strict=True).parent != canonical_root:
                    try:
                        candidate.rmdir()
                    except OSError:
                        pass
                    raise BaselineError("invalid_run_id", "run identifier escaped output root")
                return candidate, candidate_id
            except FileExistsError:
                continue
            except OSError as exc:
                raise BaselineError("output_directory_failure", "unable to create run directory") from exc
    except OSError as exc:
        raise BaselineError("invalid_output_root", "unable to create output root") from exc
    raise BaselineError("output_directory_failure", "run directory namespace is full")


def validate_loopback_host(host: str) -> None:
    if host != "127.0.0.1":
        raise BaselineError("invalid_host", "baseline host must be loopback")


def validate_port(port: int) -> None:
    if not isinstance(port, int) or isinstance(port, bool) or not 1 <= port <= 65535:
        raise BaselineError("invalid_port", "port is outside the supported range")


def port_is_available(host: str, port: int) -> bool:
    """Return whether the validated loopback endpoint can be rebound."""

    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    try:
        with socket.socket(family, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind((host, port))
            return True
    except OSError:
        return False


def probe_http(host: str, port: int, path: str, timeout_seconds: float) -> dict[str, Any]:
    started = time.monotonic_ns()
    result: dict[str, Any] = {
        "endpoint": path,
        "started_at_utc": utc_now(),
        "status": None,
        "content_type": None,
        "response_bytes": 0,
        "latency_ms": None,
        "ok": False,
        "error_category": None,
    }
    try:
        request = Request(f"http://{host}:{port}{path}", method="GET")
        with urlopen(request, timeout=timeout_seconds) as response:
            body = response.read(MAX_RESPONSE_BYTES + 1)
            result["status"] = int(response.status)
            content_type = response.headers.get("Content-Type", "")
            media_type = content_type.split(";", 1)[0].strip().lower()
            result["content_type"] = media_type[:128] if media_type else None
            result["response_bytes"] = len(body)
            result["ok"] = response.status == 200 and len(body) <= MAX_RESPONSE_BYTES
            if len(body) > MAX_RESPONSE_BYTES:
                result["error_category"] = "response_too_large"
            elif path == "/health":
                try:
                    value = json.loads(body.decode("utf-8"))
                    if (
                        not isinstance(value, dict)
                        or result["content_type"] != "application/json"
                        or value.get("status") != "ok"
                        or value.get("live") is not True
                        or value.get("phase") != "running"
                    ):
                        result["ok"] = False
                        result["error_category"] = "health_contract"
                except (UnicodeError, json.JSONDecodeError):
                    result["ok"] = False
                    result["error_category"] = "health_contract"
            elif path == "/version":
                try:
                    value = json.loads(body.decode("utf-8"))
                    version = value.get("version") if isinstance(value, dict) else None
                    if (
                        not isinstance(value, dict)
                        or result["content_type"] != "application/json"
                        or value.get("name") != "IndustrialAIServiceFramework"
                        or not isinstance(version, str)
                        or not _SAFE_VERSION.fullmatch(version)
                    ):
                        result["ok"] = False
                        result["error_category"] = "version_contract"
                except (UnicodeError, json.JSONDecodeError):
                    result["ok"] = False
                    result["error_category"] = "version_contract"
            elif path == "/metrics":
                try:
                    text = body.decode("utf-8")
                    lines = [line for line in text.splitlines() if line.strip()]
                    has_sample = any(_PROMETHEUS_SAMPLE.fullmatch(line) for line in lines)
                    if (
                        result["content_type"] != "text/plain"
                        or not lines
                        or not has_sample
                    ):
                        result["ok"] = False
                        result["error_category"] = "metrics_contract"
                except UnicodeError:
                    result["ok"] = False
                    result["error_category"] = "metrics_contract"
    except HTTPError as exc:
        result["status"] = int(exc.code)
        result["error_category"] = "http_status"
    except (URLError, TimeoutError, OSError):
        result["error_category"] = "connection_or_timeout"
    finally:
        result["latency_ms"] = round((time.monotonic_ns() - started) / 1_000_000, 3)
    return result
