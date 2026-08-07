#!/usr/bin/env python3
"""Import trusted local XYZ text into the bounded IAISF point-cloud artifact layout."""

import argparse
import hashlib
import json
import math
import os
import re
import struct
import tempfile
from pathlib import Path

MAX_ARTIFACT_ID = 128
MAX_FRAME = 128
MAX_POINTS = 100_000_000
MAX_BYTES = 1 << 30
ARTIFACT_MEDIA = "application/vnd.iaisf.pointcloud.xyz-f32le"
ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")
FRAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,128}$")


def fail(message: str) -> None:
    raise ValueError(message)


def import_pointcloud(source: Path, root: Path, artifact_id: str,
                      coordinate_frame: str, manifest: Path) -> dict:
    if not ID_RE.fullmatch(artifact_id):
        fail("artifact id is invalid")
    if not FRAME_RE.fullmatch(coordinate_frame):
        fail("coordinate frame is invalid")
    if source.is_symlink() or not source.is_file():
        fail("source must be a regular non-symlink file")
    if root.exists() and root.is_symlink():
        fail("artifact root must not be a symlink")
    root = root.resolve(strict=True)
    if not root.is_dir():
        fail("artifact root must be a directory")
    target_dir = root / "inputs" / artifact_id
    if target_dir.exists() and (target_dir.is_symlink() or not target_dir.is_dir()):
        fail("artifact target directory is unsafe")
    target_dir.mkdir(parents=True, exist_ok=True)
    data_path = target_dir / "pointcloud.xyzf32le"
    manifest_path = target_dir / "artifact.json"
    if data_path.exists() or manifest_path.exists() or manifest.exists():
        fail("artifact already exists")
    if manifest.is_symlink():
        fail("manifest output must not be a symlink")

    digest = hashlib.sha256()
    points = 0
    temp_path = None
    try:
        with tempfile.NamedTemporaryFile("wb", dir=target_dir, delete=False,
                                         prefix=".pointcloud.", suffix=".tmp") as output:
            temp_path = Path(output.name)
            with source.open("r", encoding="utf-8", errors="strict") as input_file:
                for line_number, line in enumerate(input_file, 1):
                    if not line.strip():
                        continue
                    fields = line.split()
                    if len(fields) < 3:
                        fail(f"line {line_number} has fewer than three columns")
                    try:
                        xyz = tuple(float(fields[index]) for index in range(3))
                    except (ValueError, OverflowError):
                        fail(f"line {line_number} contains a non-numeric coordinate")
                    if not all(math.isfinite(value) for value in xyz):
                        fail(f"line {line_number} contains a non-finite coordinate")
                    try:
                        packed = struct.pack("<fff", *xyz)
                    except (OverflowError, struct.error):
                        fail(f"line {line_number} is outside float32 range")
                    if not all(math.isfinite(value) for value in struct.unpack("<fff", packed)):
                        fail(f"line {line_number} is outside float32 range")
                    output.write(packed)
                    digest.update(packed)
                    points += 1
                    if points > MAX_POINTS:
                        fail("point count exceeds limit")
        if points == 0:
            fail("point cloud is empty")
        size_bytes = points * 12
        if size_bytes > MAX_BYTES:
            fail("point cloud exceeds size limit")
        artifact = {
            "artifact_id": artifact_id,
            "sha256": digest.hexdigest(),
            "size_bytes": size_bytes,
            "kind": "point_cloud",
            "media_type": ARTIFACT_MEDIA,
            "coordinate_frame": coordinate_frame,
            "unit": "mm",
            "point_count": points,
        }
        os.replace(temp_path, data_path)
        temp_path = None
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest_tmp = manifest.with_name("." + manifest.name + ".tmp")
        with manifest_tmp.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(artifact, stream, ensure_ascii=False, separators=(",", ":"),
                      sort_keys=True)
            stream.write("\n")
        os.replace(manifest_tmp, manifest)
        with manifest_path.open("w", encoding="utf-8", newline="\n") as stream:
            json.dump(artifact, stream, ensure_ascii=False, separators=(",", ":"),
                      sort_keys=True)
            stream.write("\n")
        return artifact
    except Exception:
        if temp_path is not None:
            try:
                temp_path.unlink()
            except OSError:
                pass
        if data_path.exists() and not manifest_path.exists():
            try:
                data_path.unlink()
            except OSError:
                pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--artifact-id", required=True)
    parser.add_argument("--coordinate-frame", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        artifact = import_pointcloud(args.source, args.artifact_root,
                                     args.artifact_id, args.coordinate_frame,
                                     args.manifest)
        print(json.dumps(artifact, ensure_ascii=False, separators=(",", ":"),
                         sort_keys=True))
        return 0
    except (OSError, ValueError) as error:
        print(f"import failed: {error}", file=os.sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
