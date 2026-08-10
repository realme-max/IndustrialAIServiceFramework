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

## HTTP load benchmark

在 clean worktree 上从仓库根目录运行正式 HTTP 基准：

```text
python3 benchmarks/scripts/run_http_benchmark.py
```

该入口使用 Linux Release 服务和 `127.0.0.1`，健康检查并发为
`1/8/32/128`（每组预热 5 秒、测量 30 秒），点云上传并发为 `1/2/4`。
上传内容固定为 30 MiB、786,432 行 XYZ 文本，每组取得 20 个成功样本；
canonical point-cloud size 为 9,437,184 bytes。客户端是 Python 标准库
`http.client`，与服务同机，健康请求每个 worker 使用一个 keep-alive 连接，
因此结果是 harness-specific loopback baseline，不是服务端理论最大 QPS。

正式 clean run（代码 SHA `c900e3d7cfa375c0471dac273be7635a32995a12`，run
`20260810T042933578078Z-c900e3d7cfa3-http-load`）的每个 profile 错误率为 0，
负载后 `/health` 均通过。CPU 按正式窗口内 CPU 秒/墙钟秒加权；RSS 在 JSON/CSV
中保存为 bytes，展示时使用 MiB = bytes / 1,048,576。输出字段包括请求数、成功/失败数、
请求吞吐、上传 MiB/s、P50/P95/P99/max、CPU、RSS、客户端 CPU、post-load health
和有效采样窗口数。上传 P99 每组只有 20 个样本，仅作探索性分位数。

结果目录仍只生成 `manifest.json`、`profiles.csv`、`summary.json` 和有界
`run.log`，不保存原始 payload 或 Artifact；该基准不执行 PTV2、WeldAgent、
CUDA/GPU、Job 压力或 soak test。dirty worktree 仅可显式使用 `--allow-dirty` 做开发验证，
不得将其当作正式 clean 证据。

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
