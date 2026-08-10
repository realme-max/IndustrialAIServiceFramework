import json
import sys
import threading
from unittest import mock
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from benchmark_common import BaselineError  # noqa: E402
from run_http_benchmark import (  # noqa: E402
    CSV_FIELDS,
    PAYLOAD_LINE_BYTES,
    UPLOAD_CANONICAL_SIZE_BYTES,
    UPLOAD_PAYLOAD_BYTES,
    UPLOAD_POINT_COUNT,
    _health_contract,
    _make_xyz_payload,
    _ProcessSampler,
    _require_post_load_health,
    _stop_sampler_then_probe,
    _run_workers,
    _request_health,
    _upload_contract,
    _validate_profiles,
    summarize_samples,
)


class HttpBenchmarkTest(unittest.TestCase):
    def test_health_request_uses_keep_alive_and_validates_response(self):
        class Response:
            status = 200
            headers = {"Content-Type": "application/json; charset=utf-8"}

            @staticmethod
            def read(_limit):
                return b'{"status":"ok","live":true,"phase":"running"}'

        class Connection:
            def __init__(self):
                self.headers = None

            def request(self, _method, _path, headers):
                self.headers = headers

            @staticmethod
            def getresponse():
                return Response()

        connection = Connection()
        ok, latency = _request_health(connection, 1.0)
        self.assertTrue(ok)
        self.assertGreaterEqual(latency, 0.0)
        self.assertEqual(connection.headers["Connection"], "keep-alive")

    def test_disconnect_is_counted_as_failure_without_retrying_same_request(self):
        class Disconnected:
            def __init__(self, *_args, **_kwargs):
                raise OSError("disconnected")

        with mock.patch("run_http_benchmark.http.client.HTTPConnection", Disconnected):
            result = _run_workers(
                "health", "127.0.0.1", 1, 1, 0.01, 0.02, 0, None,
                [0], threading.Lock(), set(), threading.Lock(),
            )
        self.assertGreater(result[0], 0)
        self.assertEqual(result[1], 0)
        self.assertIn("connection_failure", result[4])

    def test_profiles_are_bounded_and_reject_unknown_concurrency(self):
        self.assertEqual(_validate_profiles((1, 8, 32, 128), (1, 8, 32, 128)), (1, 8, 32, 128))
        with self.assertRaises(BaselineError):
            _validate_profiles((1, 3), (1, 8, 32, 128))
        with self.assertRaises(BaselineError):
            _validate_profiles((1, 1), (1, 8, 32, 128))

    def test_percentiles_and_throughput_are_finite(self):
        row = summarize_samples(
            "health", 8, 10.0, 10, 9, 1, [1.0, 2.0, 3.0, 4.0, 5.0],
            0, 12.5, 20.0, 100, 200, 0.25,
        )
        self.assertEqual(row["requests"], row["succeeded"] + row["failed"])
        self.assertEqual(row["throughput_requests_per_second"], 0.9)
        self.assertEqual(row["error_rate"], 0.1)
        self.assertEqual(row["latency_p50_ms"], 3.0)
        self.assertEqual(row["latency_p95_ms"], 4.8)
        self.assertIn("client_cpu_seconds", row)
        self.assertEqual(set(CSV_FIELDS), {
            "scenario", "concurrency", "duration_seconds", "requests", "succeeded", "failed",
            "uploaded_bytes",
            "throughput_requests_per_second", "throughput_mib_per_second", "latency_p50_ms",
            "latency_p95_ms", "latency_p99_ms", "latency_max_ms", "server_cpu_average_percent",
            "server_cpu_peak_percent", "server_rss_initial_bytes", "server_rss_peak_bytes",
            "client_cpu_seconds", "post_load_health_ok", "resource_sample_count",
        })

    def test_payload_is_exact_size_valid_xyz_and_unique(self):
        first, points = _make_xyz_payload(UPLOAD_PAYLOAD_BYTES, 1)
        second, second_points = _make_xyz_payload(UPLOAD_PAYLOAD_BYTES, 2)
        self.assertEqual(len(first), 31_457_280)
        self.assertEqual(len(first) % PAYLOAD_LINE_BYTES, 0)
        self.assertEqual(points, UPLOAD_POINT_COUNT)
        self.assertEqual(points, second_points)
        self.assertEqual(points * 12, UPLOAD_CANONICAL_SIZE_BYTES)
        self.assertNotEqual(first, second)
        lines = bytes(first).splitlines()
        self.assertEqual(len(lines), UPLOAD_POINT_COUNT)
        for line in (lines[0], lines[len(lines) // 2], lines[-1]):
            self.assertEqual(len(line), PAYLOAD_LINE_BYTES - 1)
            tokens = line.split()
            self.assertEqual(len(tokens), 3)
            for token in tokens:
                value = float(token.decode("ascii"))
                self.assertTrue(value == value)
                self.assertNotEqual(value, float("inf"))
                self.assertNotEqual(value, float("-inf"))
        self.assertEqual(lines[-1], lines[1])

    def test_payload_rejects_non_exact_size(self):
        with self.assertRaises(BaselineError):
            _make_xyz_payload(PAYLOAD_LINE_BYTES + 1, 1)

    def test_upload_target_reservation_is_exact_for_concurrency(self):
        class Connection:
            def __init__(self, *_args, **_kwargs):
                pass

            def close(self):
                pass

        for concurrency in (2, 4):
            ids = iter("pc_" + format(index, "064x") for index in range(20))

            def successful_upload(_connection, _body, _timeout):
                return True, 0.1, next(ids)

            with mock.patch("run_http_benchmark.http.client.HTTPConnection", Connection), \
                 mock.patch("run_http_benchmark._request_upload", successful_upload):
                result = _run_workers(
                    "upload", "127.0.0.1", 1, concurrency, 1.0, 2.0, PAYLOAD_LINE_BYTES, 20,
                    [0], threading.Lock(), set(), threading.Lock(),
                )
            self.assertEqual(result[0], 20)
            self.assertEqual(result[1], 20)
            self.assertEqual(result[0] - result[1], 0)

    def test_upload_failure_releases_slot_and_reaches_exact_success_target(self):
        class Connection:
            def __init__(self, *_args, **_kwargs):
                pass

            def close(self):
                pass

        attempts = [0]
        ids = iter("pc_" + format(index, "064x") for index in range(20))

        def one_failed_upload(_connection, _body, _timeout):
            attempts[0] += 1
            if attempts[0] == 1:
                return False, 0.1, None
            return True, 0.1, next(ids)

        with mock.patch("run_http_benchmark.http.client.HTTPConnection", Connection), \
             mock.patch("run_http_benchmark._request_upload", one_failed_upload):
            result = _run_workers(
                "upload", "127.0.0.1", 1, 2, 1.0, 2.0, PAYLOAD_LINE_BYTES, 20,
                [0], threading.Lock(), set(), threading.Lock(),
            )
        self.assertEqual(result[1], 20)
        self.assertEqual(result[0], 21)
        self.assertEqual(result[0] - result[1], 1)
        self.assertIn("response_contract", result[4])

    def test_sampler_uses_complete_time_weighted_windows_and_finishes(self):
        events = []
        clock_values = iter((0.0, 0.1, 0.4))
        usage_values = iter(((0.05, 110), (0.08, 120)))
        wait_values = iter((False, False, True))

        def wait(interval):
            events.append(("wait", interval))
            return next(wait_values)

        def read_usage(_pid):
            events.append("read")
            return next(usage_values)

        sampler = _ProcessSampler(
            123, (0.0, 100), clock=lambda: next(clock_values), wait=wait,
        )
        with mock.patch("run_http_benchmark._read_process_usage", read_usage):
            sampler.start()
            result = sampler.stop()
        self.assertFalse(sampler._thread.is_alive())
        self.assertTrue(result["valid"])
        self.assertEqual(result["sample_count"], 2)
        self.assertAlmostEqual(result["cpu_average_percent"], 20.0, places=3)
        self.assertAlmostEqual(result["cpu_peak_percent"], 50.0, places=3)
        self.assertEqual(result["rss_initial_bytes"], 100)
        self.assertEqual(result["rss_peak_bytes"], 120)
        self.assertEqual(events[0][0], "wait")
        self.assertEqual(events[1], "read")
        self.assertAlmostEqual(sampler._samples[0][1], 0.1, places=6)
        self.assertAlmostEqual(sampler._samples[1][1], 0.3, places=6)

    def test_sampler_with_fewer_than_two_windows_fails_closed(self):
        waits = iter((False, True))
        sampler = _ProcessSampler(
            123, (0.0, 100), clock=iter((0.0, 0.1)).__next__, wait=lambda _interval: next(waits),
        )
        with mock.patch("run_http_benchmark._read_process_usage", return_value=(0.05, 110)):
            sampler.start()
            result = sampler.stop()
        self.assertFalse(result["valid"])
        self.assertEqual(result["sample_count"], 1)
        self.assertFalse(sampler._thread.is_alive())

    def test_post_load_health_contract_failure_is_structured(self):
        for probe in (
            {"ok": False, "status": 503},
            {"ok": False, "status": 200, "error_category": "health_contract"},
        ):
            with mock.patch("run_http_benchmark.probe_http", return_value=probe):
                with self.assertRaises(BaselineError) as context:
                    _require_post_load_health("127.0.0.1", 1, 1.0)
            self.assertEqual(context.exception.category, "post_load_health_failure")

    def test_post_load_probe_follows_sampler_stop(self):
        events = []

        class Sampler:
            @staticmethod
            def stop():
                events.append("stop")
                return {"valid": True, "sample_count": 2}

        def probe(_host, _port, _path, _timeout):
            self.assertEqual(events, ["stop"])
            events.append("probe")
            return {
                "ok": True,
                "status": 200,
                "content_type": "application/json",
            }

        with mock.patch("run_http_benchmark.probe_http", probe):
            usage, health_ok = _stop_sampler_then_probe(Sampler(), "127.0.0.1", 1, 1.0)
        self.assertEqual(usage["sample_count"], 2)
        self.assertTrue(health_ok)
        self.assertEqual(events, ["stop", "probe"])

    def test_health_contract_rejects_invalid_status_and_schema(self):
        good = b'{"status":"ok","live":true,"phase":"running"}'
        self.assertTrue(_health_contract(200, "application/json; charset=utf-8", good))
        self.assertFalse(_health_contract(503, "application/json", good))
        self.assertFalse(_health_contract(200, "text/plain", good))
        self.assertFalse(_health_contract(200, "application/json", b"not-json"))

    def test_upload_contract_requires_strict_artifact_and_download_url(self):
        artifact_id = "pc_" + "a" * 64
        body = json.dumps({
            "schema_version": "1.0",
            "artifact": {
                "artifact_id": artifact_id,
                "sha256": "b" * 64,
                "size_bytes": 24,
                "kind": "point_cloud",
                "media_type": "application/vnd.iaisf.pointcloud.xyz-f32le",
                "coordinate_frame": "camera",
                "unit": "mm",
                "point_count": 2,
            },
            "download_url": f"/api/artifacts/v1/files/{artifact_id}",
        }).encode("utf-8")
        self.assertEqual(_upload_contract(201, "application/json", body, 24, 2), artifact_id)
        invalid = json.loads(body.decode("utf-8"))
        invalid["artifact"]["point_count"] = 3
        with self.assertRaises(BaselineError):
            _upload_contract(201, "application/json", json.dumps(invalid).encode(), 24, 2)
        with self.assertRaises(BaselineError):
            _upload_contract(200, "application/json", body, 24, 2)


if __name__ == "__main__":
    unittest.main()
