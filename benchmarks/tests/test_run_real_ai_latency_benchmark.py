import hashlib
import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "benchmarks" / "scripts"))
SCRIPT = ROOT / "benchmarks" / "scripts" / "run_real_ai_latency_benchmark.py"
SPEC = importlib.util.spec_from_file_location("real_ai_latency_benchmark", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class RealAiLatencyBenchmarkTests(unittest.TestCase):
    def test_latency_stats_are_finite_and_ordered(self):
        values = MODULE._latency_stats([1.0, 2.0, 5.0, 10.0])
        self.assertEqual(values["min"], 1.0)
        self.assertEqual(values["max"], 10.0)
        self.assertEqual(values["mean"], 4.5)
        self.assertLessEqual(values["p50"], values["p95"])
        self.assertLessEqual(values["p95"], values["p99"])

    def test_latency_stats_reject_nonfinite(self):
        with self.assertRaises(MODULE.BaselineError) as raised:
            MODULE._latency_stats([1.0, float("nan")])
        self.assertEqual(raised.exception.category, "invalid_measurement")

    def test_xyz_conversion_preserves_point_count_and_finite_values(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input.xyzf32le"
            target = root / "input.txt"
            source.write_bytes(struct.pack("<fff", 1.0, -2.5, 3.25) + struct.pack("<fff", 4.0, 5.0, 6.0))
            MODULE._write_xyz_text(source, target, 2)
            lines = target.read_text(encoding="ascii").splitlines()
            self.assertEqual(len(lines), 2)
            self.assertEqual(lines[0].split(), ["1", "-2.5", "3.25"])
            self.assertEqual(lines[1].split(), ["4", "5", "6"])

    def test_xyz_conversion_rejects_truncation_and_cleans(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "bad.xyzf32le"
            target = root / "bad.txt"
            source.write_bytes(b"\0" * 11)
            with self.assertRaises(MODULE.BaselineError) as raised:
                MODULE._write_xyz_text(source, target, 1)
            self.assertEqual(raised.exception.category, "input_binary")
            self.assertFalse(target.exists())

    def test_artifact_contract_requires_eight_fields_and_canonical_values(self):
        artifact = {
            "artifact_id": "pc_" + "a" * 64,
            "sha256": "b" * 64,
            "size_bytes": 24,
            "kind": "point_cloud",
            "media_type": "application/vnd.iaisf.pointcloud.xyz-f32le",
            "coordinate_frame": "camera",
            "unit": "mm",
            "point_count": 2,
        }
        self.assertEqual(MODULE._validate_artifact(artifact, expected_size=24, expected_points=2), artifact)
        with self.assertRaises(MODULE.BaselineError):
            MODULE._validate_artifact({**artifact, "extra": 1}, expected_size=24, expected_points=2)

    def test_submit_contract_rejects_wrong_status_url(self):
        response = {"job_id": "wi_" + "a" * 32, "status_url": "/wrong"}
        with mock.patch.object(MODULE, "_request", return_value=(202, "application/json", json.dumps(response).encode(), 1.0)):
            artifact = {
                "artifact_id": "pc_" + "a" * 64,
                "sha256": "b" * 64,
                "size_bytes": 24,
                "kind": "point_cloud",
                "media_type": "application/vnd.iaisf.pointcloud.xyz-f32le",
                "coordinate_frame": "camera",
                "unit": "mm",
                "point_count": 2,
            }
            with self.assertRaises(MODULE.BaselineError) as raised:
                MODULE._submit("127.0.0.1", 1, "weld_inspection", artifact)
            self.assertEqual(raised.exception.category, "submit_contract")

    def test_poll_rejects_unknown_state(self):
        body = {"job_id": "wi_" + "a" * 32, "application": "weld_inspection", "state": "mystery"}
        with mock.patch.object(MODULE, "_request", return_value=(200, "application/json", json.dumps(body).encode(), 1.0)):
            with self.assertRaises(MODULE.BaselineError) as raised:
                MODULE._poll("127.0.0.1", 1, "weld_inspection", body["job_id"])
            self.assertEqual(raised.exception.category, "status_contract")

    def test_ptv2_result_contract_matches_public_serializer(self):
        result = {
            "schema_version": "1.0", "job_id": "wi_" + "a" * 32,
            "application": "weld_inspection", "state": "succeeded", "version": 4,
            "output_artifacts": [{"artifact_id": "x"}, {"artifact_id": "y"}],
            "weld_points": {"artifact_id": "x"}, "prediction": {"artifact_id": "y"},
            "weld_point_count": 205,
            "weld_ratio": 205 / 2048, "length_mm": 0.8,
            "inference_time_ms": 1.0, "total_time_ms": 2.0,
            "quality_assessment": "not_implemented",
        }
        self.assertEqual(len(MODULE._verify_ptv2(result, 2048, result["job_id"])), 2)
        with self.assertRaises(MODULE.BaselineError):
            MODULE._verify_ptv2({**result, "quality_assessment": "implemented"}, 2048, result["job_id"])

    def test_weldagent_public_result_is_allowlist_projected(self):
        result = {
            "schema_version": "1.0", "job_id": "wg_" + "a" * 32,
            "application": "welding_guidance", "state": "succeeded", "version": 4,
            "output_artifacts": [{"artifact_id": "a"}], "weld_type": "straight",
            "coordinate_frame": "camera", "unit": "mm", "start": [0.0, 0.0, 0.0],
            "end": [1.0, 0.0, 0.0], "x_axis": [1.0, 0.0, 0.0],
            "y_axis": [0.0, 1.0, 0.0], "z_axis": [0.0, 0.0, 1.0],
            "confidence": 0.9, "disposition": "completed",
            "robot_execution_allowed": False,
        }
        self.assertEqual(len(MODULE._verify_weldagent(result, result["job_id"])), 1)
        with self.assertRaises(MODULE.BaselineError):
            MODULE._verify_weldagent({**result, "joint_values": [1, 2, 3]}, result["job_id"])

    def test_download_requires_canonical_url_and_exact_sha(self):
        artifact = {
            "artifact_id": "result-json", "sha256": hashlib.sha256(b"{}").hexdigest(),
            "size_bytes": 2, "media_type": "application/json",
            "download_url": "/api/artifacts/v1/files/result-json",
        }
        with mock.patch.object(MODULE, "_request", return_value=(200, "application/json", b"{}", 2.0)):
            elapsed, body = MODULE._download("127.0.0.1", 1, artifact)
        self.assertEqual(elapsed, 2.0)
        self.assertEqual(body, b"{}")
        with self.assertRaises(MODULE.BaselineError):
            MODULE._download("127.0.0.1", 1, {**artifact, "download_url": "https://example.invalid/result-json"})

    def test_prepare_config_disables_external_paths_in_service_roots(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template = root / "config.json"
            template.write_text(json.dumps({"server": {"tcp": {}}, "http": {"limits": {}}, "applications": {}}), encoding="utf-8")
            payload = MODULE._prepare_config(template, "127.0.0.1", 18480, root / "work", root / "runtime")
            value = json.loads(payload)
            self.assertTrue(value["applications"]["enabled"])
            self.assertEqual(value["server"]["port"], 18480)
            self.assertEqual(value["applications"]["weld_agent"]["project_root"], str(root / "runtime"))

    def test_runtime_copy_rejects_symlink_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            try:
                (source / "link").symlink_to(root / "missing")
            except OSError:
                self.skipTest("symlink capability is unavailable on this Windows host")
            tool = root / "tool.json"
            tool.write_text('{"allow_send_url": false}', encoding="utf-8")
            with self.assertRaises(MODULE.BaselineError) as raised:
                MODULE._prepare_runtime(source, tool, root / "destination")
            self.assertEqual(raised.exception.category, "weldagent_runtime_invalid")


if __name__ == "__main__":
    unittest.main()
