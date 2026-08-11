from __future__ import annotations

import json
import importlib.util
import os
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "benchmarks" / "scripts"))

from run_application_job_stress import (  # noqa: E402
    BaselineError,
    PROFILE_ATTEMPTS,
    QUEUE_CAPACITY,
    REPOSITORY_CAPACITY,
    SUBMIT_WORKERS,
    _artifact_payload,
    _batch_submission_metrics,
    _copy_fixture_to_work_root,
    _complete_drain_diagnostic,
    _capture_stall_snapshot,
    _capture_stall_snapshot_once,
    _drain_row_defaults,
    _effective_config,
    _json_response,
    _latencies,
    _method_metadata,
    _status,
    _state_counts,
    _workspace_aggregate,
    _proc_thread_snapshot,
    _submit_on_connection,
    _update_drain_snapshot,
    _submit_many,
    _run_profile,
    sha256_file,
    _validate_fixture_executable,
    _validate_profiles,
)


class ApplicationJobStressTest(unittest.TestCase):
    def test_stall_snapshot_is_single_shot_and_bounded(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "markers" / "invocation_started").mkdir(parents=True)
            (root / "markers" / "invocation_finished").mkdir()
            (root / "markers" / "invocation_started" / "000001").write_text("1\n", encoding="ascii")
            (root / "outputs").mkdir()
            (root / "scratch").mkdir()
            (root / "outputs" / "weld_result.json").write_text("{}", encoding="ascii")
            row = {"failure_category": "drain_timeout"}
            row.update(_drain_row_defaults())
            process = mock.Mock()
            process.pid = 12345
            process.poll.return_value = None
            sampler = mock.Mock()
            sampler._thread.is_alive.return_value = True
            with mock.patch("run_application_job_stress._proc_thread_snapshot", return_value={"available": False, "error": "capability_unavailable"}), \
                 mock.patch("run_application_job_stress.probe_http", return_value={"ok": True}), \
                 mock.patch("run_application_job_stress.shutil.which", return_value=None):
                tracker = {"taken": False}
                kwargs = dict(run_dir=root, host="127.0.0.1", port=19000, attempts=250,
                              drain_started=10.0, completed=4, remaining=125,
                              states=["succeeded"] * 4 + ["queued"] * 125,
                              last_progress_at=11.0, process=process, sampler=sampler,
                              work_root=root, kind="stall")
                self.assertTrue(_capture_stall_snapshot_once(tracker, row, **kwargs))
                self.assertFalse(_capture_stall_snapshot_once(tracker, row, **kwargs))
            self.assertEqual(row["drain_stall_snapshot_count"], 1)
            self.assertLess(len(row["drain_stall_snapshot_json"]), 32768)
            self.assertNotIn("wi_", row["drain_stall_snapshot_json"])
            self.assertNotIn("http", row["drain_stall_snapshot_json"])

    def test_workspace_aggregate_counts_lifecycle_and_output_without_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for name in ("invocation_started", "invocation_finished"):
                (root / "markers" / name).mkdir(parents=True)
            (root / "markers" / "invocation_started" / "000001").write_text("1", encoding="ascii")
            (root / "markers" / "invocation_started" / "000002").write_text("1", encoding="ascii")
            (root / "markers" / "invocation_finished" / "000001").write_text("1", encoding="ascii")
            (root / "outputs" / "job").mkdir(parents=True)
            (root / "outputs" / "job" / "weld_result.json").write_text("{}", encoding="ascii")
            (root / "outputs" / "job" / "prediction.txt").write_text("0\n", encoding="ascii")
            (root / "scratch").mkdir()
            (root / "scratch" / "cloud.txt").write_text("x", encoding="ascii")
            (root / "artifacts").mkdir()
            (root / "artifacts" / "artifact.json").write_text("{}", encoding="ascii")
            aggregate = _workspace_aggregate(root)
            self.assertEqual(aggregate["fixture_started_count"], 2)
            self.assertEqual(aggregate["fixture_finished_count"], 1)
            self.assertEqual(aggregate["fixture_inflight_count"], 1)
            self.assertEqual(aggregate["output_result_file_count"], 1)
            self.assertEqual(aggregate["output_prediction_file_count"], 1)
            self.assertEqual(aggregate["scratch_file_count"], 1)
            self.assertEqual(aggregate["manifest_count"], 1)

    def test_proc_snapshot_capability_is_explicitly_unavailable_on_non_posix(self):
        with mock.patch("run_application_job_stress.os.name", "nt"):
            self.assertEqual(_proc_thread_snapshot(123), {
                "available": False, "error": "capability_unavailable"
            })

    def test_fixture_marker_conflict_allocates_next_fixed_width_sequence(self):
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        spec = importlib.util.spec_from_file_location("mock_ptv2_marker_fixture", fixture)
        self.assertIsNotNone(spec)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            markers = Path(temp) / "markers"
            markers.mkdir()
            (markers / "invocation_started").mkdir()
            (markers / "invocation_started" / "000001").write_text("1", encoding="ascii")
            claimed = module._claim_invocation_marker(markers, "invocation_started")
            self.assertEqual(claimed.name, "000002")

    def test_fixture_marker_creation_failure_is_fail_closed(self):
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        spec = importlib.util.spec_from_file_location("mock_ptv2_marker_failure_fixture", fixture)
        self.assertIsNotNone(spec)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "outputs" / "jobs" / "wi_test" / "ptv2"
            output.mkdir(parents=True)
            (root / "markers").mkdir()
            cloud = root / "scratch" / "cloud.txt"
            cloud.parent.mkdir()
            cloud.write_text("0 0 0 0\n", encoding="ascii")
            engine = root / "engine.bin"
            plugin = root / "plugin.so"
            engine.write_bytes(b"x")
            plugin.write_bytes(b"x")
            with mock.patch.object(module, "_claim_invocation_marker", return_value=None):
                result = module.main(["--engine", str(engine), "--plugin", str(plugin),
                                      "--cloud", str(cloud), "--output", str(output)])
            self.assertEqual(result, 5)
            self.assertFalse((output / "weld_result.json").exists())
    def test_fixed_profile_and_capacity_contract(self):
        self.assertEqual(_validate_profiles(PROFILE_ATTEMPTS), (100, 250, 500))
        self.assertEqual((QUEUE_CAPACITY, REPOSITORY_CAPACITY, SUBMIT_WORKERS), (128, 1024, 64))
        with self.assertRaises(Exception):
            _validate_profiles((100, 200, 500))

    def test_payload_is_small_legal_xyz(self):
        self.assertEqual(_artifact_payload(), b"0 0 0\n")

    def test_percentiles_are_bounded_and_finite(self):
        self.assertEqual(_latencies([1.0, 2.0, 3.0]), (2.0, 2.9, 2.98, 3.0))
        self.assertEqual(_latencies([]), (None, None, None, None))

    def test_batch_submission_metrics_exclude_blocker_wait(self):
        measured, duration, rate = _batch_submission_metrics(249, 100.0, 100.2)
        self.assertEqual(measured, 249)
        self.assertAlmostEqual(duration, 0.2)
        self.assertAlmostEqual(rate, 1245.0)
        with self.assertRaises(BaselineError) as context:
            _batch_submission_metrics(0, 100.0, 100.2)
        self.assertEqual(context.exception.category, "invalid_measurement")

    def test_method_scope_is_explicit(self):
        method = _method_metadata()
        self.assertEqual(method["submission_throughput_scope"], "accepted and queue_full HTTP responses")
        self.assertIn("LocalProcessRunner fork/exec", method["drain_scope"])
        self.assertEqual(method["resource_scope"], "IAISF server PID only; synthetic child processes excluded")
        self.assertTrue(method["not_queue_only_throughput"])
        self.assertTrue(method["not_application_acceptance_capacity"])
        self.assertTrue(method["not_ai_performance"])

    def test_drain_state_aggregation_and_progress_snapshot(self):
        self.assertEqual(
            _state_counts(["queued", "running", "succeeded", "unexpected"]),
            {"queued": 1, "dispatching": 0, "running": 1, "succeeded": 1,
             "cancelling": 0, "waiting_human": 0, "failed": 0, "cancelled": 0,
             "timed_out": 0, "worker_lost": 0, "other": 1},
        )
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "markers").mkdir()
            for marker in ("blocker_claimed", "started", "release"):
                (root / "markers" / marker).write_text("1\n", encoding="ascii")
            process = mock.Mock()
            process.poll.return_value = None
            sampler = mock.Mock()
            sampler._thread.is_alive.return_value = True
            row = _drain_row_defaults()
            with mock.patch("run_application_job_stress.time.monotonic", return_value=12.0):
                _update_drain_snapshot(
                    row, attempts=250, drain_started=10.0, completed=3, remaining=126,
                    states=["succeeded", "succeeded", "succeeded"] + ["queued"] * 126,
                    last_progress_at=11.0, process=process, sampler=sampler,
                    work_root=root,
                )
            self.assertEqual(row["drain_completed_jobs"], 3)
            self.assertEqual(row["drain_remaining_jobs"], 126)
            self.assertEqual(row["drain_last_progress_elapsed_seconds"], 1.0)
            self.assertEqual(row["drain_stalled_seconds"], 1.0)
            self.assertEqual(row["drain_state_succeeded"], 3)
            self.assertEqual(row["drain_state_queued"], 126)
            self.assertTrue(row["drain_server_alive"])
            self.assertTrue(row["drain_sampler_ok"])

    def test_drain_timeout_diagnostic_preserves_partial_measurement_and_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "markers").mkdir()
            for marker in ("blocker_claimed", "started", "release"):
                (root / "markers" / marker).write_text("1\n", encoding="ascii")
            process = mock.Mock()
            process.poll.return_value = None
            sampler = mock.Mock()
            sampler._thread.is_alive.return_value = True
            row = {"failure_category": "drain_timeout"}
            row.update(_drain_row_defaults())
            with mock.patch("run_application_job_stress.probe_http", return_value={"ok": True}):
                _complete_drain_diagnostic(
                    row, host="127.0.0.1", port=19000, attempts=500,
                    drain_started=10.0, completed=4, remaining=125,
                    states=["succeeded"] * 4 + ["queued"] * 125,
                    last_progress_at=11.0, process=process, sampler=sampler,
                    work_root=root,
                )
            self.assertEqual(row["failure_category"], "drain_timeout")
            self.assertEqual(row["drain_completed_jobs"], 4)
            self.assertEqual(row["drain_remaining_jobs"], 125)
            self.assertTrue(row["drain_health_ok"])
            self.assertTrue(row["drain_diagnostic_complete"])
            self.assertEqual(row["drain_timeout_attempts_profile"], 500)

    def test_drain_diagnostic_failure_is_separate_from_original_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "markers").mkdir()
            process = mock.Mock()
            process.poll.return_value = None
            sampler = mock.Mock()
            sampler._thread.is_alive.return_value = True
            row = {"failure_category": "drain_timeout"}
            row.update(_drain_row_defaults())
            with mock.patch("run_application_job_stress.probe_http", side_effect=OSError("unavailable")):
                _complete_drain_diagnostic(
                    row, host="127.0.0.1", port=19000, attempts=250,
                    drain_started=10.0, completed=0, remaining=129,
                    states=["queued"] * 129, last_progress_at=10.0,
                    process=process, sampler=sampler, work_root=root,
                )
            self.assertEqual(row["failure_category"], "drain_timeout")
            self.assertFalse(row["drain_diagnostic_complete"])
            self.assertEqual(row["drain_diagnostic_error"], "diagnostic_unavailable")

    def test_fixture_not_executable_fails_closed(self):
        with tempfile.TemporaryDirectory() as temp:
            fixture = Path(temp) / "fixture.py"
            fixture.write_text("#!/usr/bin/env python3\n", encoding="ascii")
            with mock.patch.object(os, "name", "posix"), mock.patch.object(os, "access", return_value=False):
                with self.assertRaises(BaselineError) as context:
                    _validate_fixture_executable(fixture)
            self.assertEqual(context.exception.category, "fixture_not_executable")

    def test_expected_backpressure_counts(self):
        for attempts in PROFILE_ATTEMPTS:
            accepted = min(attempts, QUEUE_CAPACITY + 1)
            self.assertEqual((accepted, attempts - accepted),
                             (100, 0) if attempts == 100 else
                             (129, 121) if attempts == 250 else (129, 371))

    def test_mock_fixture_rejects_missing_required_arguments(self):
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        result = subprocess.run(
            [sys.executable, str(fixture)], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False,
        )
        self.assertNotEqual(result.returncode, 0)

    def test_mock_fixture_accepts_arguments_and_writes_minimal_result(self):
        if os.name != "posix":
            self.skipTest("fixture executable path is a Linux capability")
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "outputs" / "jobs" / "wi_test" / "ptv2"
            output.mkdir(parents=True)
            markers = root / "markers"
            markers.mkdir()
            (markers / "blocker_claimed").write_text("1\n", encoding="ascii")
            cloud = root / "scratch" / "cloud.txt"
            cloud.parent.mkdir()
            cloud.write_text("0 0 0 0\n", encoding="ascii")
            engine = root / "engine.bin"
            plugin = root / "plugin.so"
            engine.write_bytes(b"x")
            plugin.write_bytes(b"x")
            completed = subprocess.run(
                [str(fixture), "--engine", str(engine), "--plugin", str(plugin),
                 "--cloud", str(cloud), "--output", str(output)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                check=False, timeout=5,
            )
            self.assertEqual(completed.returncode, 0)
            result = json.loads((output / "weld_result.json").read_text(encoding="utf-8"))
            self.assertEqual(result["total_points"], 1)
            self.assertTrue((output / "prediction.txt").is_file())

    def test_mock_fixture_blocker_started_release_and_no_output_noise(self):
        if os.name != "posix":
            self.skipTest("fixture executable path is a Linux capability")
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "outputs" / "jobs" / "wi_test" / "ptv2"
            output.mkdir(parents=True)
            markers = root / "markers"
            markers.mkdir()
            cloud = root / "scratch" / "cloud.txt"
            cloud.parent.mkdir()
            cloud.write_text("0 0 0 0\n", encoding="ascii")
            engine = root / "engine.bin"
            plugin = root / "plugin.so"
            engine.write_bytes(b"x")
            plugin.write_bytes(b"x")
            command = [str(fixture), "--engine", str(engine), "--plugin", str(plugin),
                       "--cloud", str(cloud), "--output", str(output)]
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            deadline = time.monotonic() + 5.0
            while not (markers / "started").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue((markers / "started").is_file())
            self.assertIsNone(process.poll())
            (markers / "release").write_text("release\n", encoding="ascii")
            stdout, stderr = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 0)
            self.assertEqual(stdout, b"")
            self.assertEqual(stderr, b"")
            self.assertTrue((output / "weld_result.json").is_file())

            second_output = root / "outputs" / "jobs" / "wi_second" / "ptv2"
            completed = subprocess.run(
                [str(fixture), "--engine", str(engine), "--plugin", str(plugin),
                 "--cloud", str(cloud), "--output", str(second_output)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=5,
            )
            self.assertEqual(completed.returncode, 0)
            self.assertEqual(completed.stdout, b"")
            self.assertEqual(completed.stderr, b"")

    def test_mock_fixture_timeout_seam_does_not_wait_thirty_seconds(self):
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        spec = importlib.util.spec_from_file_location("mock_ptv2_fixture", fixture)
        self.assertIsNotNone(spec)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            ticks = iter((0.0, 0.1, 1.0))
            sleeps = []
            self.assertFalse(module._wait_for_marker(
                Path(temp) / "release", clock=lambda: next(ticks),
                sleeper=lambda seconds: sleeps.append(seconds), timeout=0.5,
            ))
            self.assertEqual(sleeps, [0.01])

    def test_mock_fixture_blocker_timeout_returns_nonzero_without_waiting(self):
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        spec = importlib.util.spec_from_file_location("mock_ptv2_timeout_fixture", fixture)
        self.assertIsNotNone(spec)
        module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "outputs" / "jobs" / "wi_test" / "ptv2"
            output.mkdir(parents=True)
            markers = root / "markers"
            markers.mkdir()
            cloud = root / "scratch" / "cloud.txt"
            cloud.parent.mkdir()
            cloud.write_text("0 0 0 0\n", encoding="ascii")
            engine = root / "engine.bin"
            plugin = root / "plugin.so"
            engine.write_bytes(b"x")
            plugin.write_bytes(b"x")
            result = module.main(
                ["--engine", str(engine), "--plugin", str(plugin), "--cloud", str(cloud),
                 "--output", str(output)],
                wait_timeout=0.0,
            )
            self.assertEqual(result, 3)
            self.assertTrue((markers / "started").is_file())
            self.assertFalse((output / "weld_result.json").exists())

    def test_mock_fixture_rejects_engine_plugin_and_cloud_escape(self):
        if os.name != "posix":
            self.skipTest("fixture executable path is a Linux capability")
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output = root / "outputs" / "jobs" / "wi_test" / "ptv2"
            output.mkdir(parents=True)
            markers = root / "markers"
            markers.mkdir()
            cloud = root / "scratch" / "cloud.txt"
            cloud.parent.mkdir()
            cloud.write_text("0 0 0 0\n", encoding="ascii")
            engine = root / "engine.bin"
            plugin = root / "plugin.so"
            engine.write_bytes(b"x")
            plugin.write_bytes(b"x")
            outside = root.parent / f"{root.name}-outside"
            for name, replacement in (("engine", outside / "engine.bin"),
                                      ("plugin", outside / "plugin.so"),
                                      ("cloud", outside / "cloud.txt")):
                args = [str(fixture), "--engine", str(engine), "--plugin", str(plugin),
                        "--cloud", str(cloud), "--output", str(output)]
                args[args.index(f"--{name}") + 1] = str(replacement)
                completed = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                            check=False, timeout=5)
                self.assertNotEqual(completed.returncode, 0)
            self.assertFalse(outside.exists())
            self.assertFalse((output / "weld_result.json").exists())

    def test_native_fixture_copy_is_bounded_hashed_and_executable(self):
        if os.name != "posix":
            self.skipTest("native POSIX fixture copy is a platform capability")
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        with tempfile.TemporaryDirectory() as temp:
            work_root = Path(temp)
            target = _copy_fixture_to_work_root(fixture, work_root)
            self.assertEqual(target.parent, work_root / "bin")
            self.assertFalse(target.is_symlink())
            self.assertEqual(sha256_file(fixture), sha256_file(target))
            self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o700)
            self.assertTrue(os.access(target, os.X_OK))

    def test_fixture_source_symlink_and_copy_target_escape_fail_closed(self):
        if os.name != "posix":
            self.skipTest("native POSIX fixture copy is a platform capability")
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source_link = root / "source-link.py"
            try:
                source_link.symlink_to(fixture)
            except OSError as exc:
                self.skipTest(f"symlink capability unavailable: {exc}")
            with self.assertRaises(BaselineError) as source_error:
                _copy_fixture_to_work_root(source_link, root / "work")
            self.assertEqual(source_error.exception.category, "fixture_source_invalid")

            work_root = root / "work"
            work_root.mkdir()
            outside = root / "outside"
            with self.assertRaises(BaselineError) as escape_error:
                _copy_fixture_to_work_root(fixture, work_root, outside / "copy.py")
            self.assertEqual(escape_error.exception.category, "fixture_copy_failure")
            self.assertFalse((outside / "copy.py").exists())

    def test_fixture_copy_target_symlink_or_directory_is_rejected(self):
        if os.name != "posix":
            self.skipTest("native POSIX fixture copy is a platform capability")
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            target = bin_dir / fixture.name
            target.mkdir()
            with self.assertRaises(BaselineError) as directory_error:
                _copy_fixture_to_work_root(fixture, root)
            self.assertEqual(directory_error.exception.category, "fixture_copy_failure")
            target.rmdir()
            try:
                target.symlink_to(fixture)
            except OSError as exc:
                self.skipTest(f"symlink capability unavailable: {exc}")
            with self.assertRaises(BaselineError) as symlink_error:
                _copy_fixture_to_work_root(fixture, root)
            self.assertEqual(symlink_error.exception.category, "fixture_copy_failure")

    def test_fixture_copy_failure_cleans_profile_work_root(self):
        if os.name != "posix":
            self.skipTest("native POSIX fixture copy is a platform capability")
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source_link = root / "source-link.py"
            try:
                source_link.symlink_to(fixture)
            except OSError as exc:
                self.skipTest(f"symlink capability unavailable: {exc}")
            work_root = root / "owned-work-root"

            def create_work_root(*_args, **_kwargs):
                work_root.mkdir()
                return str(work_root)

            with mock.patch("run_application_job_stress.tempfile.mkdtemp", side_effect=create_work_root):
                row, cleanup = _run_profile(
                    repo_root=ROOT,
                    server_path=Path("/usr/bin/env"),
                    template=root / "unused-template.json",
                    fixture=source_link,
                    run_dir=root / "results",
                    host="127.0.0.1",
                    port=19091,
                    attempts=100,
                )
            self.assertEqual(row["failure_category"], "fixture_source_invalid")
            self.assertTrue(cleanup["fixture_copy_removed"])
            self.assertTrue(cleanup["work_root_removed"])

    def test_latency_split_supports_queue_full_and_accepted(self):
        all_values = [1.0, 2.0, 3.0]
        self.assertEqual(_latencies(all_values)[0], 2.0)

    def test_submit_parses_accepted_and_structured_queue_full(self):
        class Response:
            def __init__(self, status, body):
                self.status = status
                self.headers = {"Content-Type": "application/json; charset=utf-8"}
                self._body = body

            def read(self, _limit):
                return self._body

        class Connection:
            def __init__(self, response):
                self.response = response

            def request(self, *_args, **_kwargs):
                return None

            def getresponse(self):
                return self.response

        artifact = {
            "artifact_id": "pc_" + "a" * 64,
            "sha256": "a" * 64,
            "size_bytes": 12,
            "kind": "point_cloud",
            "media_type": "application/vnd.iaisf.pointcloud.xyz-f32le",
            "coordinate_frame": "camera",
            "unit": "mm",
            "point_count": 1,
        }
        accepted = Connection(Response(202, json.dumps({
            "job_id": "wi_" + "b" * 32,
            "status_url": "/api/weld-inspection/v1/jobs/wi_" + "b" * 32,
        }).encode()))
        self.assertEqual(_submit_on_connection(accepted, artifact)[3], "accepted")
        queue_full = Connection(Response(503, b'{"error":{"code":"queue_full","message":"busy"}}'))
        self.assertEqual(_submit_on_connection(queue_full, artifact)[3], "queue_full")

        malformed_accepted = [
            {"job_id": "bad", "status_url": "/api/weld-inspection/v1/jobs/bad"},
            {"job_id": "wi_" + "c" * 32, "status_url": "/wrong/path"},
        ]
        for body in malformed_accepted:
            with self.subTest(body=body), self.assertRaises(BaselineError) as context:
                _submit_on_connection(Connection(Response(202, json.dumps(body).encode())), artifact)
            self.assertEqual(context.exception.category, "submit_contract")

        for status, body in (
            (400, {"error": {"code": "bad_request", "message": "bad"}}),
            (500, {"error": {"code": "internal", "message": "bad"}}),
            (503, {"error": {"code": "busy", "message": "bad"}}),
        ):
            with self.subTest(status=status), self.assertRaises(BaselineError) as context:
                _submit_on_connection(Connection(Response(status, json.dumps(body).encode())), artifact)
            self.assertEqual(context.exception.category, "submit_unexpected")

    def test_json_response_and_disconnect_fail_closed(self):
        class Response:
            def __init__(self, content_type, body):
                self.status = 200
                self.headers = {"Content-Type": content_type}
                self.body = body

            def read(self, _limit):
                return self.body

        for response in (
            Response("text/plain", b"{}"),
            Response("application/json", b"not-json"),
            Response("application/json", b"x" * ((1 << 20) + 1)),
        ):
            with self.subTest(content_type=response.headers["Content-Type"]), self.assertRaises(BaselineError) as context:
                _json_response(response)
            self.assertEqual(context.exception.category, "invalid_http_response")

        class DisconnectingConnection:
            def request(self, *_args, **_kwargs):
                raise ConnectionError("disconnect")

            def close(self):
                return None

        with mock.patch("run_application_job_stress.http.client.HTTPConnection", return_value=DisconnectingConnection()):
            result = _submit_many("127.0.0.1", 1, {}, 3)
        self.assertEqual(len(result["all_latencies"]), 3)
        self.assertEqual(result["errors"], ["submit_failure"] * 3)

    def test_status_rejects_unknown_state_and_identity_mismatch(self):
        class Response:
            headers = {"Content-Type": "application/json"}

            def __init__(self, body):
                self.status = 200
                self.body = json.dumps(body).encode()

            def read(self, _limit):
                return self.body

        class Connection:
            def __init__(self, response):
                self.response = response

            def request(self, *_args, **_kwargs):
                return None

            def getresponse(self):
                return self.response

            def close(self):
                return None

        job = "wi_" + "a" * 32
        url = f"/api/weld-inspection/v1/jobs/{job}"
        base = {
            "schema_version": "1.0", "job_id": job,
            "application": "weld_inspection", "phase": "post_weld",
            "state": "running", "version": 2, "created_at": 1,
            "updated_at": 2, "status_url": url,
        }
        for changes in (
            {"state": "unknown"},
            {"job_id": "wi_" + "b" * 32},
            {"application": "welding_guidance"},
            {"phase": "pre_weld"},
            {"status_url": "/wrong"},
        ):
            body = dict(base)
            body.update(changes)
            with self.subTest(changes=changes), mock.patch(
                "run_application_job_stress.http.client.HTTPConnection",
                return_value=Connection(Response(body)),
            ), self.assertRaises(BaselineError) as context:
                _status("127.0.0.1", 1, url, job)
            self.assertEqual(context.exception.category, "status_contract")

    def test_status_contract_is_fail_closed(self):
        class Response:
            status = 200
            headers = {"Content-Type": "application/json"}

            def read(self, _limit):
                return json.dumps({
                    "schema_version": "1.0", "job_id": "wi_" + "a" * 32,
                    "application": "weld_inspection", "phase": "post_weld",
                    "state": "running", "version": 2, "created_at": 1,
                    "updated_at": 2,
                    "status_url": "/api/weld-inspection/v1/jobs/wi_" + "a" * 32,
                }).encode()

        class Connection:
            def request(self, *_args, **_kwargs):
                return None

            def getresponse(self):
                return Response()

            def close(self):
                return None

        job = "wi_" + "a" * 32
        url = f"/api/weld-inspection/v1/jobs/{job}"
        with mock.patch("run_application_job_stress.http.client.HTTPConnection", return_value=Connection()):
            self.assertEqual(_status("127.0.0.1", 1, url, job), "running")

    def test_effective_config_contains_fixed_queue_and_synthetic_scope(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            template = root / "template.json"
            template.write_text('{"schema_version":1,"server":{},"logging":{"level":"info"}}', encoding="utf-8")
            fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
            data = json.loads(_effective_config(template, "127.0.0.1", 19000, root / "work", fixture))
            self.assertTrue(data["applications"]["enabled"])
            self.assertEqual(data["applications"]["queue_capacity"], 128)
            self.assertEqual(data["applications"]["repository_capacity"], 1024)

    def test_profile_start_failure_cleans_owned_work_root(self):
        fixture = ROOT / "benchmarks" / "fixtures" / "mock_ptv2_cli.py"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            work_root = root / "owned-work-root"
            def create_work_root(*_args, **_kwargs):
                work_root.mkdir()
                return str(work_root)

            with mock.patch("run_application_job_stress.tempfile.mkdtemp", side_effect=create_work_root):
                row, cleanup = _run_profile(
                    repo_root=ROOT,
                    server_path=root / "missing-server",
                    template=root / "unused-template.json",
                    fixture=fixture,
                    run_dir=root / "results",
                    host="127.0.0.1",
                    port=19090,
                    attempts=100,
                )
            self.assertEqual(row["failure_category"], "executable_missing")
            self.assertTrue(cleanup["work_root_removed"])
            self.assertTrue(cleanup["markers_removed"])
            self.assertTrue(cleanup["artifacts_removed"])
            self.assertTrue(cleanup["port_released"])


if __name__ == "__main__":
    unittest.main()
