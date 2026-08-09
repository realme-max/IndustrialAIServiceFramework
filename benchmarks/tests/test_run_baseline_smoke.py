import json
import os
import signal
import socket
import sys
import tempfile
import textwrap
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from benchmark_common import BaselineError  # noqa: E402
from run_baseline_smoke import run_baseline_smoke  # noqa: E402


FAKE_SERVER = textwrap.dedent(
    r'''
    import argparse
    import json
    import os
    import signal
    import sys
    import threading
    from http.server import BaseHTTPRequestHandler, HTTPServer

    parser = argparse.ArgumentParser()
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--config", required=True)
    parser.add_argument("--noise", action="store_true")
    parser.add_argument("--noise-stderr", action="store_true")
    parser.add_argument("--bad-health", action="store_true")
    parser.add_argument("--bad-version", action="store_true")
    parser.add_argument("--bad-metrics", action="store_true")
    parser.add_argument("--non-json", action="store_true")
    parser.add_argument("--large", action="store_true")
    parser.add_argument("--http-error", action="store_true")
    parser.add_argument("--child", action="store_true")
    parser.add_argument("--ignore-term", action="store_true")
    args = parser.parse_args()
    if args.noise:
        print("x" * 70000, flush=True)
    if args.noise_stderr:
        print("y" * 70000, file=sys.stderr, flush=True)
    config = json.loads(open(args.config, encoding="utf-8").read())
    host = config["server"]["host"]
    port = config["server"]["port"]

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/health":
                body = (
                    b'not-json' if args.non_json else
                    b'{"status":"ok","live":true,"ready":true,"phase":"running"}'
                )
                if args.bad_health:
                    body = b'{"status":"ok","live":false,"ready":true,"phase":"running"}'
                content_type = "application/json"
            elif self.path == "/version":
                body = b'{"name":"IndustrialAIServiceFramework","version":"fixture"}'
                if args.bad_version:
                    body = b'{"name":"other-service","version":"fixture"}'
                content_type = "application/json"
            elif self.path == "/metrics":
                body = b'# TYPE fixture counter\nfixture 1\n'
                content_type = "application/json" if args.bad_metrics else "text/plain; version=0.0.4"
            else:
                body = b'{}'
                content_type = "application/json"
            if args.large and self.path == "/health":
                body = b'{"status":"ok","live":true,"ready":true,"phase":"running"}' + b"x" * 1100000
            status = 503 if args.http_error and self.path == "/health" else (200 if self.path in {"/health", "/version", "/metrics"} else 404)
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Content-Type", content_type)
            self.end_headers()
            self.wfile.write(body)
            if self.path == "/metrics" and not args.ignore_term:
                if child_pid is not None:
                    os.kill(child_pid, signal.SIGTERM)
                    os.waitpid(child_pid, 0)
                os._exit(0)

        def log_message(self, *_args):
            pass

    child_pid = None
    if args.child and hasattr(os, "fork"):
        child_pid = os.fork()
        if child_pid == 0:
            threading.Event().wait()
        child_pid = child_pid if child_pid > 0 else None
    server = HTTPServer((host, port), Handler)
    signal.signal(signal.SIGTERM, signal.SIG_IGN if args.ignore_term else (lambda *_args: os._exit(0)))

    server.serve_forever()
    '''
)


class BaselineSmokeTest(unittest.TestCase):
    def _fixture(self, root: Path) -> tuple[Path, Path]:
        server = root / "fixture_server.py"
        server.write_text(FAKE_SERVER, encoding="utf-8")
        if os.name == "posix":
            server.chmod(0o755)
        config = root / "template.json"
        config.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "server": {"host": "127.0.0.1", "port": 18181},
                    "metrics": {"enabled": True, "endpoint": "/metrics"},
                }
            ),
            encoding="utf-8",
        )
        return server, config

    def test_success_produces_replayable_sanitized_artifacts(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            output = root_path / "results"
            run_dir, summary = run_baseline_smoke(
                repo_root=Path(__file__).resolve().parents[2],
                server_path=server,
                config_template=config,
                build_type="Release",
                output_root=output,
                host="127.0.0.1",
                port=18182,
                startup_timeout_seconds=5,
                request_timeout_seconds=1,
                allow_dirty=True,
                server_command=[sys.executable, str(server)],
            )
            self.assertEqual(summary["outcome"], "success")
            self.assertEqual([row["endpoint"] for row in summary["requests"]], ["/health", "/version", "/metrics"])
            self.assertEqual(
                sorted(path.name for path in run_dir.iterdir()),
                ["manifest.json", "run.log", "samples.csv", "summary.json"],
            )
            manifest = json.loads((run_dir / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["schema_version"], "1.0")
            self.assertEqual(manifest["parameters"]["applications_enabled"], False)
            self.assertEqual(
                set(manifest),
                {
                    "schema_version",
                    "run_id",
                    "scenario",
                    "started_at_utc",
                    "finished_at_utc",
                    "git_sha",
                    "git_dirty",
                    "build_configuration",
                    "server_sha256",
                    "config_sha256",
                    "environment",
                    "parameters",
                    "input",
                    "outcome",
                    "failure_category",
                },
            )
            self.assertRegex(manifest["started_at_utc"], r"^\d{4}-\d{2}-\d{2}T.*Z$")
            self.assertRegex(manifest["finished_at_utc"], r"^\d{4}-\d{2}-\d{2}T.*Z$")
            self.assertEqual(
                set(json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))),
                {
                    "schema_version",
                    "run_id",
                    "scenario",
                    "outcome",
                    "failure_category",
                    "process_exit_code",
                    "requests",
                    "process_output",
                    "cleanup",
                },
            )
            artifact_text = "\n".join(
                path.read_text(encoding="utf-8") for path in run_dir.iterdir()
            )
            self.assertNotIn(str(root_path), artifact_text)
            self.assertNotIn(str(server), artifact_text)
            self.assertNotIn("PATH", artifact_text)
            self.assertNotIn("HOME", artifact_text)
            self.assertNotIn(sys.executable, artifact_text)
            self.assertTrue(summary["cleanup"]["process_reaped"])
            self.assertTrue(summary["cleanup"]["temporary_config_removed"])
            self.assertTrue(summary["cleanup"]["port_released"])
            self.assertTrue(summary["cleanup"]["process_group_reaped"])
            self.assertTrue(summary["cleanup"]["capture_threads_finished"])
            self.assertTrue(summary["cleanup"]["result_temp_files_removed"])
            self.assertEqual(
                (run_dir / "samples.csv").read_text(encoding="utf-8").splitlines()[0],
                "endpoint,started_at_utc,status,content_type,response_bytes,latency_ms,ok,error_category",
            )

    def test_repeated_runs_get_distinct_directories(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            output = root_path / "results"
            kwargs = dict(
                repo_root=Path(__file__).resolve().parents[2],
                server_path=server,
                config_template=config,
                build_type="Release",
                output_root=output,
                host="127.0.0.1",
                port=18183,
                startup_timeout_seconds=5,
                request_timeout_seconds=1,
                allow_dirty=True,
                server_command=[sys.executable, str(server)],
            )
            first, _ = run_baseline_smoke(**kwargs)
            second, _ = run_baseline_smoke(**kwargs)
            self.assertNotEqual(first.name, second.name)

    def test_dirty_gate_is_structured_failure(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            with mock.patch(
                "run_baseline_smoke.git_snapshot",
                return_value={"sha": "0" * 40, "dirty": True},
            ):
                _, summary = run_baseline_smoke(
                    repo_root=Path(__file__).resolve().parents[2],
                    server_path=server,
                    config_template=config,
                    build_type="Release",
                    output_root=root_path / "results",
                    host="127.0.0.1",
                    port=18184,
                    startup_timeout_seconds=5,
                    request_timeout_seconds=1,
                    allow_dirty=False,
                    server_command=[sys.executable, str(server)],
                )
            # The dirty snapshot is injected so this gate remains tested after
            # the baseline checkpoint itself becomes a clean worktree.
            self.assertEqual(summary["outcome"], "failure")
            self.assertEqual(summary["failure_category"], "git_dirty")

    def test_invalid_invocation_parameters_fail_closed(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            common = dict(
                repo_root=Path(__file__).resolve().parents[2],
                server_path=server,
                config_template=config,
                build_type="Release",
                output_root=root_path / "results",
                host="127.0.0.1",
                port=18185,
                startup_timeout_seconds=5,
                request_timeout_seconds=1,
                allow_dirty=True,
                server_command=[sys.executable, str(server)],
            )
            for field, value, category in (
                ("build_type", "Profile", "invalid_build_type"),
                ("host", "0.0.0.0", "invalid_host"),
                ("port", 0, "invalid_port"),
                ("startup_timeout_seconds", 0, "invalid_timeout"),
                ("request_timeout_seconds", 61, "invalid_timeout"),
            ):
                with self.subTest(field=field):
                    kwargs = dict(common)
                    kwargs[field] = value
                    with self.assertRaises(BaselineError) as context:
                        run_baseline_smoke(**kwargs)
                    self.assertEqual(context.exception.category, category)

    def test_invalid_template_and_startup_failure_write_structured_summary(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            config.write_text("{not-json", encoding="utf-8")
            run_dir, summary = run_baseline_smoke(
                repo_root=Path(__file__).resolve().parents[2],
                server_path=server,
                config_template=config,
                build_type="Release",
                output_root=root_path / "invalid-config-results",
                host="127.0.0.1",
                port=18186,
                startup_timeout_seconds=5,
                request_timeout_seconds=1,
                allow_dirty=True,
                server_command=[sys.executable, str(server)],
            )
            self.assertEqual(summary["failure_category"], "config_invalid")
            self.assertTrue((run_dir / "summary.json").is_file())

            _, summary = run_baseline_smoke(
                repo_root=Path(__file__).resolve().parents[2],
                server_path=server,
                config_template=self._fixture(root_path)[1],
                build_type="Release",
                output_root=root_path / "startup-results",
                host="127.0.0.1",
                port=18187,
                startup_timeout_seconds=0.2,
                request_timeout_seconds=0.2,
                allow_dirty=True,
                server_command=[sys.executable, "-c", "import threading; threading.Event().wait()"],
            )
            self.assertIn(summary["failure_category"], {"startup_timeout", "process_exit"})
            self.assertTrue(summary["cleanup"]["process_reaped"])

    def test_missing_server_and_output_root_are_structured_failures(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            missing_server = root_path / "missing-server"
            _, summary = run_baseline_smoke(
                repo_root=Path(__file__).resolve().parents[2],
                server_path=missing_server,
                config_template=config,
                build_type="Release",
                output_root=root_path / "missing-server-results",
                host="127.0.0.1",
                port=18189,
                startup_timeout_seconds=5,
                request_timeout_seconds=1,
                allow_dirty=True,
                server_command=[sys.executable, str(server)],
            )
            self.assertEqual(summary["failure_category"], "server_missing")

            invalid_root = root_path / "output-file"
            invalid_root.write_text("not a directory", encoding="utf-8")
            with self.assertRaises(BaselineError) as context:
                run_baseline_smoke(
                    repo_root=Path(__file__).resolve().parents[2],
                    server_path=server,
                    config_template=config,
                    build_type="Release",
                    output_root=invalid_root,
                    host="127.0.0.1",
                    port=18190,
                    startup_timeout_seconds=5,
                    request_timeout_seconds=1,
                    allow_dirty=True,
                    server_command=[sys.executable, str(server)],
                )
            self.assertEqual(context.exception.category, "invalid_output_root")

    def test_process_output_limit_is_bounded_and_structured(self):
        for index, flag in enumerate(("--noise", "--noise-stderr")):
            with self.subTest(flag=flag), tempfile.TemporaryDirectory() as root:
                root_path = Path(root)
                server, config = self._fixture(root_path)
                _, summary = run_baseline_smoke(
                    repo_root=Path(__file__).resolve().parents[2],
                    server_path=server,
                    config_template=config,
                    build_type="Release",
                    output_root=root_path / "results",
                    host="127.0.0.1",
                    port=18188 + index,
                    startup_timeout_seconds=5,
                    request_timeout_seconds=1,
                    allow_dirty=True,
                    server_command=[sys.executable, str(server), flag],
                )
                self.assertEqual(summary["outcome"], "failure")
                self.assertEqual(summary["failure_category"], "process_output_limit")
                self.assertLessEqual(summary["process_output"]["stdout_bytes"], 65537)
                self.assertLessEqual(summary["process_output"]["stderr_bytes"], 65537)
                self.assertTrue(summary["process_output"]["bounded"])

    def test_occupied_port_is_rejected_before_process_start(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            marker = root_path / "started-marker"
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as occupied:
                occupied.bind(("127.0.0.1", 0))
                occupied.listen(1)
                port = occupied.getsockname()[1]
                _, summary = run_baseline_smoke(
                    repo_root=Path(__file__).resolve().parents[2],
                    server_path=server,
                    config_template=config,
                    build_type="Release",
                    output_root=root_path / "results",
                    host="127.0.0.1",
                    port=port,
                    startup_timeout_seconds=5,
                    request_timeout_seconds=1,
                    allow_dirty=True,
                    server_command=[
                        sys.executable,
                        "-c",
                        f"from pathlib import Path; Path({str(marker)!r}).write_text('started')",
                    ],
                )
            self.assertEqual(summary["failure_category"], "port_in_use")
            self.assertEqual(summary["requests"], [])
            self.assertFalse(marker.exists())
            self.assertTrue(summary["cleanup"]["temporary_config_removed"])

    def test_http_contract_errors_are_fail_closed(self):
        cases = (
            ("--bad-health", "health_contract", "/health"),
            ("--bad-version", "version_contract", "/version"),
            ("--bad-metrics", "metrics_contract", "/metrics"),
            ("--non-json", "health_contract", "/health"),
            ("--large", "response_too_large", "/health"),
            ("--http-error", "http_status", "/health"),
        )
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            for index, (flag, category, endpoint) in enumerate(cases):
                with self.subTest(flag=flag):
                    _, summary = run_baseline_smoke(
                        repo_root=Path(__file__).resolve().parents[2],
                        server_path=server,
                        config_template=config,
                        build_type="Release",
                        output_root=root_path / f"results-{index}",
                        host="127.0.0.1",
                        port=18196 + index,
                        startup_timeout_seconds=2,
                        request_timeout_seconds=0.2,
                        allow_dirty=True,
                        server_command=[sys.executable, str(server), flag],
                    )
                    self.assertEqual(summary["failure_category"], category)
                    requests = summary["requests"]
                    if requests:
                        matching = [item for item in requests if item["endpoint"] == endpoint]
                        if matching:
                            self.assertEqual(matching[0]["error_category"], category)
                    self.assertNotIn("not-json", " ".join(
                        path.read_text(encoding="utf-8")
                        for path in (root_path / f"results-{index}").rglob("*")
                        if path.is_file()
                    ))

    @unittest.skipUnless(os.name == "posix", "process-group fixture requires POSIX")
    def test_process_group_child_is_reaped(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            _, summary = run_baseline_smoke(
                repo_root=Path(__file__).resolve().parents[2],
                server_path=server,
                config_template=config,
                build_type="Release",
                output_root=root_path / "results",
                host="127.0.0.1",
                port=18203,
                startup_timeout_seconds=5,
                request_timeout_seconds=1,
                allow_dirty=True,
                server_command=[sys.executable, str(server), "--child"],
            )
            self.assertEqual(summary["outcome"], "success")
            self.assertTrue(summary["cleanup"]["process_group_reaped"])

    @unittest.skipUnless(os.name == "posix", "SIGKILL fallback requires POSIX")
    def test_shutdown_timeout_uses_kill_and_reports_cleanup(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            _, summary = run_baseline_smoke(
                repo_root=Path(__file__).resolve().parents[2],
                server_path=server,
                config_template=config,
                build_type="Release",
                output_root=root_path / "results",
                host="127.0.0.1",
                port=18204,
                startup_timeout_seconds=5,
                request_timeout_seconds=1,
                allow_dirty=True,
                server_command=[sys.executable, str(server), "--ignore-term"],
            )
            self.assertEqual(summary["process_exit_code"], -signal.SIGKILL)
            self.assertTrue(summary["cleanup"]["process_group_reaped"])

    def test_cleanup_failure_cannot_remain_success(self):
        with tempfile.TemporaryDirectory() as root:
            root_path = Path(root)
            server, config = self._fixture(root_path)
            original_unlink = Path.unlink

            def deny_config_unlink(path: Path, missing_ok: bool = False):
                if path.name == ".config.json":
                    raise OSError("injected cleanup failure")
                return original_unlink(path, missing_ok=missing_ok)

            with mock.patch.object(Path, "unlink", deny_config_unlink):
                run_dir, summary = run_baseline_smoke(
                    repo_root=Path(__file__).resolve().parents[2],
                    server_path=server,
                    config_template=config,
                    build_type="Release",
                    output_root=root_path / "results",
                    host="127.0.0.1",
                    port=18205,
                    startup_timeout_seconds=5,
                    request_timeout_seconds=1,
                    allow_dirty=True,
                    server_command=[sys.executable, str(server)],
                )
            self.assertEqual(summary["outcome"], "failure")
            self.assertEqual(summary["failure_category"], "cleanup_failure")
            self.assertFalse(summary["cleanup"]["temporary_config_removed"])
            self.assertTrue((run_dir / ".config.json").exists())


if __name__ == "__main__":
    unittest.main()
