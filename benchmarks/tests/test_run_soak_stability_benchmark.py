import csv
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "benchmarks" / "scripts"))
SCRIPT = ROOT / "benchmarks" / "scripts" / "run_soak_stability_benchmark.py"
SPEC = importlib.util.spec_from_file_location("soak_stability_benchmark", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class SoakStabilityBenchmarkTests(unittest.TestCase):
    def test_duration_and_cadence_map_to_exact_cycle_counts(self):
        self.assertEqual(MODULE._validate_duration_and_cadence(600, 120), 5)
        self.assertEqual(MODULE._validate_duration_and_cadence(7200, 120), 60)
        self.assertEqual(MODULE._validate_duration_and_cadence(43200, 120), 360)
        with self.assertRaises(MODULE.BaselineError) as raised:
            MODULE._validate_duration_and_cadence(601, 120)
        self.assertEqual(raised.exception.category, "invalid_duration")
        with self.assertRaises(MODULE.BaselineError) as raised:
            MODULE._validate_duration_and_cadence(600, 60)
        self.assertEqual(raised.exception.category, "invalid_cadence")

    def test_latency_summary_rejects_nonfinite_and_preserves_first_last_windows(self):
        records = []
        for elapsed in (1.0, 3599.0, 3601.0, 599.0):
            records.append({
                "record_type": "job", "application": "weld_inspection",
                "elapsed_seconds": elapsed, "final_state": "succeeded",
                "submit_latency_ms": elapsed, "terminal_latency_ms": elapsed + 1,
                "result_latency_ms": elapsed + 2, "download_latency_ms": elapsed + 3,
                "total_latency_ms": elapsed + 4,
            })
        summary = MODULE._job_latency_summary(records, "weld_inspection", 7200)
        self.assertEqual(summary["jobs"], 4)
        self.assertEqual(summary["successes"], 4)
        self.assertEqual(summary["submit_latency_ms_first_hour"]["min_ms"], 1.0)
        self.assertEqual(summary["submit_latency_ms_last_hour"]["min_ms"], 3601.0)
        records[0]["submit_latency_ms"] = float("nan")
        with self.assertRaises(MODULE.BaselineError):
            MODULE._job_latency_summary(records, "weld_inspection", 7200)

    def test_resource_summary_reports_trends_and_external_children(self):
        samples = [
            {"elapsed_seconds": 0.0, "cpu_seconds": 1.0, "rss_bytes": 100,
             "thread_count": 4, "fd_count": 8, "runtime_disk_bytes": 10,
             "external_child_count": 0},
            {"elapsed_seconds": 10.0, "cpu_seconds": 2.0, "rss_bytes": 120,
             "thread_count": 5, "fd_count": 9, "runtime_disk_bytes": 20,
             "external_child_count": 1},
            {"elapsed_seconds": 3700.0, "cpu_seconds": 3.0, "rss_bytes": 110,
             "thread_count": 4, "fd_count": 8, "runtime_disk_bytes": 15,
             "external_child_count": 0},
        ]
        result = MODULE._resource_summary(samples, 7200.0)
        self.assertEqual(result["sample_count"], 3)
        self.assertEqual(result["rss_bytes"]["initial"], 100)
        self.assertEqual(result["rss_bytes"]["final"], 110)
        self.assertEqual(result["external_child_count_max"], 1)
        self.assertAlmostEqual(result["rss_bytes"]["per_hour_change"], 5.0, places=3)

    def test_resource_summary_rejects_missing_or_nonfinite_samples(self):
        with self.assertRaises(MODULE.BaselineError) as raised:
            MODULE._resource_summary([], 10.0)
        self.assertEqual(raised.exception.category, "resource_sampling_failure")
        sample = {"elapsed_seconds": 0.0, "cpu_seconds": 1.0, "rss_bytes": 100,
                  "thread_count": 4, "fd_count": 8, "runtime_disk_bytes": float("nan")}
        with self.assertRaises(MODULE.BaselineError):
            MODULE._resource_summary([sample], 10.0)

    def test_sample_writer_has_fixed_header_flushes_and_rejects_existing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "samples.csv"
            writer = MODULE._SampleWriter(path)
            writer.append({"record_type": "cycle", "cycle_index": 1, "health_ok": True})
            self.assertTrue(writer.close())
            with path.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(rows[0]["record_type"], "cycle")
            self.assertEqual(rows[0]["health_ok"], "true")
            self.assertEqual(tuple(rows[0].keys()), MODULE.CSV_FIELDS)
            with self.assertRaises(MODULE.BaselineError) as raised:
                MODULE._SampleWriter(path)
            self.assertEqual(raised.exception.category, "output_exists")

    def test_append_event_is_bounded_and_does_not_log_sensitive_fields(self):
        lines = []
        MODULE._append_event(
            lines, "cycle_completed", cycle=1, job_id="secret", url="/private",
            path="C:/private", command="secret command", stdout="raw", stderr="raw",
            detail="x" * 500,
        )
        self.assertEqual(len(lines), 1)
        self.assertLessEqual(len(lines[0]), 200)
        self.assertNotIn("secret", lines[0])
        self.assertNotIn("private", lines[0])
        self.assertNotIn("raw", lines[0])

    def test_directory_size_does_not_follow_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "data").write_bytes(b"1234")
            try:
                (root / "link").symlink_to(root / "data")
            except OSError:
                self.skipTest("symlink capability is unavailable")
            self.assertEqual(MODULE._directory_size(root), 4)

    def test_manifest_method_is_explicit_about_scope(self):
        method = {
            "resource_scope": "IAISF server PID only; PTV2/WeldAgent child processes and GPU excluded",
            "not_concurrent_load": True,
            "not_ai_performance": True,
        }
        self.assertTrue(method["not_concurrent_load"])
        self.assertTrue(method["not_ai_performance"])
        self.assertIn("server PID only", method["resource_scope"])

    def test_soak_config_keeps_repository_capacity_at_1024(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template = root / "config.json"
            template.write_text("{}", encoding="utf-8")
            payload = MODULE._prepare_soak_config(
                template, "127.0.0.1", 18490, root / "work", root / "runtime"
            )
            value = json.loads(payload)
            self.assertEqual(value["applications"]["repository_capacity"], 1024)
            self.assertEqual(value["applications"]["queue_capacity"], 8)


if __name__ == "__main__":
    unittest.main()
