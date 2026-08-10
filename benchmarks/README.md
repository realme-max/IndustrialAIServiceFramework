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

## 标准加固验证

HEAD `e049d9bd5c46dd65be2ea1f0feb98a408d9b8e7a` 的 WSL Ubuntu 24.04 / GCC
13.3 验证已完成：独立 `build/linux-asan-ubsan` 使用
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`
完成完整 CTest，930 registered、929 passed、1 个既有权限能力 skip、0 failed，
未发现 ASan、LeakSanitizer 或 UBSan 报告。普通 `build/linux-release` 串行执行
`ctest --repeat until-fail:50`；该命令要求每个已执行测试连续成功 50 次，任一测试失败时立即停止。结果为
930/929/1/0，墙钟 735 秒。唯一 skip 是
`SafePathResolverTest.PermissionFailureIsExplicitlyHandled`；WSL 的 clock-skew
输出是环境时间戳提示，不计入项目 warning。尚未执行 TSan、Valgrind、压力测试、
真实 AI 性能测试或 soak test。
