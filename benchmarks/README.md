# Reproducible baseline smoke

This directory contains the dependency-free foundation for later benchmark
work. It is a bounded health/version/metrics smoke, not a performance claim.
It never invokes PTV2, WeldAgent, CUDA, external network services, load tools,
or long-running stability tests.

## One-command Linux Release smoke

After building the Release server with the repository's normal script, run from
the repository root:

```text
python3 benchmarks/scripts/run_baseline_smoke.py
```

The command uses `build/linux-release/iaisf_server`, the local smoke template,
loopback, and an exclusive ignored result directory. Override those inputs with
`--server`, `--config-template`, `--output-root`, `--host`, and `--port` when a
different local build is intentional. The default is fail-closed on a dirty
worktree; use `--allow-dirty` only for development validation, not as the
formal clean-worktree command.

Each run produces exactly `manifest.json`, `samples.csv`, `summary.json`, and a
bounded event-only `run.log`. The manifest records schema version, run ID,
scenario, UTC times, full Git SHA and dirty state, build configuration, server
and effective-config SHA-256, a sanitized OS name/version, kernel/CPU,
logical CPU count, total memory, compiler/CMake/Python snapshot, loopback
parameters, and the no-input summary. It never records a user name, absolute
path, environment, command line, model/tool path, token,
URL credential, or raw stdout/stderr.

The fixed CSV columns are:

```text
endpoint,started_at_utc,status,content_type,response_bytes,latency_ms,ok,error_category
```

Every run enables only `/health`, `/version`, and `/metrics`; applications and
diagnostics are disabled. The server is stopped with SIGTERM and its exact
process group is reaped; the summary records process, temporary-config and
loopback-port cleanup. A failed startup or probe still writes a bounded
structured summary when an output run directory was created.

## Local Python tests

```text
python -m unittest discover -s benchmarks/tests -p "test_*.py" -v
```

The tests use a private loopback fixture and the standard library only. They do
not claim real performance, GPU, external-project, or browser evidence.

Result directories are ignored; retain only deliberately selected, sanitized
human reports outside this directory. Remove only an exact run directory known
to belong to the current invocation.
