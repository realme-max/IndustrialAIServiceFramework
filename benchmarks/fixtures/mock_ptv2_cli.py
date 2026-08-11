#!/usr/bin/env python3
"""Bounded synthetic PTV2 process used only by the queue stress benchmark.

The fixture deliberately implements the production adapter's four arguments,
but performs no inference.  The first invocation holds the application worker
until the benchmark creates ``release``; later invocations complete quickly.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path


WAIT_SECONDS = 30.0
MARKER_WIDTH = 6
MARKER_LIMIT = 1000000


def _inside(path: Path, root: Path) -> bool:
    try:
        path.resolve(strict=False).relative_to(root.resolve(strict=True))
        return True
    except (OSError, ValueError):
        return False


def _exclusive(path: Path, data: str = "1\n") -> bool:
    try:
        with path.open("x", encoding="ascii") as stream:
            stream.write(data)
        return True
    except FileExistsError:
        return False


def _claim_invocation_marker(markers: Path, directory_name: str) -> Path | None:
    """Claim one fixed-width invocation marker without exposing job identity."""
    directory = markers / directory_name
    try:
        directory.mkdir(exist_ok=True)
        for sequence in range(1, MARKER_LIMIT + 1):
            candidate = directory / f"{sequence:0{MARKER_WIDTH}d}"
            try:
                with candidate.open("x", encoding="ascii") as stream:
                    stream.write("1\n")
                return candidate
            except FileExistsError:
                continue
            except OSError:
                return None
    except OSError:
        return None
    return None


def _wait_for_marker(
    marker: Path,
    clock=time.monotonic,
    sleeper=time.sleep,
    timeout: float = WAIT_SECONDS,
) -> bool:
    deadline = clock() + timeout
    while not marker.exists() and clock() < deadline:
        sleeper(0.01)
    return marker.exists()


def main(argv: list[str] | None = None, *, wait_timeout: float = WAIT_SECONDS) -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--plugin", required=True)
    parser.add_argument("--cloud", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)

    output = Path(args.output)
    root = output.resolve(strict=False).parents[3]
    markers = root / "markers"
    if not markers.is_dir():
        return 2
    paths = [Path(args.engine), Path(args.plugin), Path(args.cloud), output]
    if any(not _inside(path, root) for path in paths):
        return 2
    started_invocation = _claim_invocation_marker(markers, "invocation_started")
    if started_invocation is None:
        return 5
    cloud = Path(args.cloud)
    try:
        lines = [line for line in cloud.read_text(encoding="utf-8").splitlines() if line.strip()]
        if not lines or any(len(line.split()) != 4 for line in lines):
            return 2
    except (OSError, UnicodeError, ValueError):
        return 2

    blocker = markers / "blocker_claimed"
    started = markers / "started"
    release = markers / "release"
    if _exclusive(blocker):
        if not _exclusive(started):
            return 2
        if not _wait_for_marker(release, timeout=wait_timeout):
            return 3

    try:
        output.mkdir(parents=True, exist_ok=True)
        total_points = len(lines)
        (output / "weld_result.json").write_text(
            json.dumps(
                {
                    "total_points": total_points,
                    "weld_points": total_points,
                    "weld_ratio": 1.0,
                    "length_mm": 1.0,
                    "inference_ms": 1.0,
                    "total_ms": 1.0,
                },
                separators=(",", ":"),
            ),
            encoding="utf-8",
        )
        (output / "prediction.txt").write_text(
            "\n".join("0" for _ in range(total_points)) + "\n", encoding="ascii"
        )
    except (OSError, UnicodeError, ValueError):
        return 4
    if _claim_invocation_marker(markers, "invocation_finished") is None:
        return 5
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
