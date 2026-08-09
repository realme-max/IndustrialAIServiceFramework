import csv
import hashlib
import json
import re
import socket
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from benchmark_common import (  # noqa: E402
    BaselineError,
    atomic_write_json,
    atomic_write_bytes,
    atomic_write_csv,
    create_unique_run_directory,
    environment_snapshot,
    make_run_id,
    utc_now,
    validate_loopback_host,
    validate_port,
)


class BenchmarkCommonTest(unittest.TestCase):
    def test_run_id_is_short_and_stable_shape(self):
        run_id = make_run_id("a" * 40, "baseline-smoke")
        self.assertRegex(run_id, r"^20[0-9]{6}T[0-9]{12}Z-aaaaaaaaaaaa-baseline-smoke$")

    def test_run_directory_claim_is_exclusive_and_does_not_delete(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            existing = root_path / "run"
            existing.mkdir()
            marker = existing / "marker.txt"
            marker.write_text("keep", encoding="utf-8")
            run_dir, run_id = create_unique_run_directory(root_path, "run")
            self.assertNotEqual(run_id, "run")
            self.assertEqual(marker.read_text(encoding="utf-8"), "keep")
            self.assertEqual(run_dir.parent, root_path)

    def test_run_directory_rejects_escape_and_path_like_ids_before_creation(self):
        invalid_ids = [
            "../escaped",
            "a/b",
            r"a\b",
            ".",
            "..",
            r"C:\\absolute",
            r"\\server\share",
            "https://example.invalid/run",
            "bad\x00id",
            "x" * 129,
        ]
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            for index, invalid_id in enumerate(invalid_ids):
                output_root = root_path / f"root-{index}"
                escaped = root_path / "escaped"
                with self.subTest(invalid_id=repr(invalid_id)):
                    with self.assertRaises(BaselineError) as context:
                        create_unique_run_directory(output_root, invalid_id)
                    self.assertEqual(context.exception.category, "invalid_run_id")
                    self.assertFalse(output_root.exists())
                    self.assertFalse(escaped.exists())

    def test_run_directory_rejects_symlink_output_root(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            target = root_path / "target"
            target.mkdir()
            link = root_path / "link"
            try:
                link.symlink_to(target, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"directory symlink capability unavailable: {error}")
            with self.assertRaises(BaselineError) as context:
                create_unique_run_directory(link, "safe-run")
            self.assertEqual(context.exception.category, "invalid_output_root")
            self.assertEqual(list(target.iterdir()), [])

    def test_atomic_write_is_no_clobber_under_concurrent_race(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "race.json"

            def write(index: int):
                try:
                    atomic_write_bytes(path, f"writer-{index}".encode("ascii"))
                    return "success"
                except BaselineError as error:
                    return error.category

            with ThreadPoolExecutor(max_workers=8) as pool:
                outcomes = list(pool.map(write, range(8)))
            self.assertEqual(outcomes.count("success"), 1)
            self.assertEqual(outcomes.count("output_exists"), 7)
            self.assertIn(path.read_text(encoding="ascii"), {f"writer-{i}" for i in range(8)})
            self.assertEqual(list(path.parent.glob(".*.tmp")), [])

    def test_atomic_json_is_utf8_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "summary.json"
            atomic_write_json(path, {"schema_version": "1.0", "message": "ok"})
            self.assertEqual(path.read_bytes()[-1:], b"\n")
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), hashlib.sha256(path.read_bytes()).hexdigest())
            with self.assertRaises(BaselineError) as context:
                atomic_write_json(path, {"message": "changed"})
            self.assertEqual(context.exception.category, "output_exists")

    def test_loopback_and_port_validation_fail_closed(self):
        validate_loopback_host("127.0.0.1")
        validate_port(18181)
        with self.assertRaises(BaselineError):
            validate_loopback_host("0.0.0.0")
        with self.assertRaises(BaselineError):
            validate_loopback_host("localhost")
        with self.assertRaises(BaselineError):
            validate_loopback_host("::1")
        with self.assertRaises(BaselineError):
            validate_port(0)
        with self.assertRaises(BaselineError):
            validate_port(True)

    def test_environment_schema_is_exact_sanitized_and_memory_is_total(self):
        snapshot = environment_snapshot()
        self.assertEqual(
            set(snapshot),
            {
                "os_name",
                "os_version",
                "kernel",
                "machine",
                "cpu_model",
                "logical_cpu_count",
                "memory_total_bytes",
                "compiler_version",
                "cmake_version",
                "python_version",
            },
        )
        encoded = json.dumps(snapshot, ensure_ascii=True)
        self.assertNotIn("available_memory_bytes", encoded)
        self.assertNotIn("HOME", encoded)
        self.assertNotIn("PATH", encoded)
        self.assertNotIn("/mnt/", encoded)
        self.assertNotIn("\\\\", encoded)
        for value in snapshot.values():
            if isinstance(value, str):
                self.assertNotRegex(value, r"[\x00-\x1f\x7f]")
                self.assertLessEqual(len(value), 160)

    def test_utc_and_csv_contract_are_fixed_and_reparseable(self):
        self.assertRegex(utc_now(), r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "samples.csv"
            atomic_write_csv(
                path,
                [{"endpoint": "/x", "ok": True, "empty": None}],
                ("endpoint", "ok", "empty"),
            )
            with path.open(encoding="utf-8", newline="") as stream:
                rows = list(csv.reader(stream))
            self.assertEqual(rows, [["endpoint", "ok", "empty"], ["/x", "true", ""]])


if __name__ == "__main__":
    unittest.main()
