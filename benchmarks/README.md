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

## Application Job 队列压力基准

正式 clean-worktree 命令（仅运行一次）：

```text
python3 benchmarks/scripts/run_application_job_stress.py \
  --server build/linux-release/iaisf_server \
  --config-template benchmarks/configs/baseline-smoke.json \
  --fixture benchmarks/fixtures/mock_ptv2_cli.py \
  --output-root benchmarks/results
```

正式 run `20260811T015906503761Z-2daf658b2acf-application-job-stress` 绑定代码 SHA
`2daf658b2acfa16594c9bcbf555c1a1e5da0971e`，`git_dirty=false`。每个 profile 使用
queue 128、Repository 1024、单 Application worker 和 64 个 HTTP submit worker；阻塞
fixture 释放前验证 1 running 与其余 queued，随后验证全部 accepted drain、recovery 和
post-load health。结果目录只保留 `manifest.json`、`profiles.csv`、`summary.json`、`run.log`，
不保留 Job、Artifact、原始输入、命令行、路径或进程输出。

| attempts | accepted | queue_full | batch submit req/s | drain s / jobs/s | CPU avg/peak | RSS 初始/峰值 MiB |
|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100 | 0 | 1586.503 | 3.016 / 33.156 | 15.761% / 29.955% | 7.066 / 8.269 |
| 250 | 129 | 121 | 2632.628 | 3.731 / 34.573 | 16.525% / 29.926% | 7.105 / 8.490 |
| 500 | 129 | 371 | 2558.032 | 3.749 / 34.409 | 16.441% / 29.933% | 7.089 / 8.488 |

batch submit throughput 的分子是 `attempts - 1`，不含 blocker 等待；drain/s 是包含
LocalProcessRunner fork/exec、synthetic fixture、Adapter 文件处理、Repository transition
和 HTTP polling 的单 worker 端到端排空，不是纯队列 pop/s 或 Application 理论接纳容量。
CPU/RSS 只统计 IAISF server PID，synthetic child 不计入；CPU 的 100% 表示一个逻辑 CPU
核心，RSS 以 bytes 保存、展示换算为 MiB。该结果是 synthetic framework queue stress，
不是 PTV2/WeldAgent 推理、GPU/CUDA、真实 AI E2E 或服务端理论最大吞吐。

PR #14 已合并的 ProcessRunner 生命周期修复和 `system_clock` 回拨终态化修复是本基准的
正确性前提；此前 20 次/60 profiles 的开发稳定性序列是历史证据，不是本次 clean run。
真实 PTV2/WeldAgent 性能与 soak test 保留为后续工作。

## 真实 Industrial AI 单 Job 延迟基准

使用 `python3 benchmarks/scripts/run_real_ai_latency_benchmark.py`，在 clean worktree
上针对真实 PTV2 和 WeldAgent 各串行执行 2 个 warmup、20 个 measured jobs。运行结果仅
保留 `manifest.json`、`profiles.csv`、`summary.json` 和有界 `run.log`；不提交输入、
Artifact、模型、配置或原始结果。

正式 run `20260811T031038135968Z-9580cc476ad7-real-ai-latency` 绑定代码 SHA
`9580cc476ad7c70b005517f9f541691044d7f2c9`，`git_dirty=false`；环境为 WSL Ubuntu 24.04、
GCC 13.3、Linux Release。PTV2 输入为 2048 点/24,576 canonical bytes，WeldAgent 输入
为 823,114 点/9,877,368 canonical bytes。客户端是同机 Python 标准库 `http.client`，
因此属于 harness-specific loopback 基线；每个分位数只有 20 个样本，P95/P99 仅作探索性
参考。资源统计只覆盖 IAISF server PID，CPU 使用加权平均，RSS 以 bytes 保存并以
`MiB = bytes / 1,048,576` 展示。

| 应用 | upload ms | submit P50/P95/P99 ms | terminal P50/P95/P99 ms | result P50/P95/P99 ms | download P50/P95/P99 ms | total P50/P95/P99 ms | CPU avg/peak | RSS 初始/峰值 MiB | 错误 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| PTV2 | 39.370 | 0.435/0.888/1.003 | 1039.477/1060.787/1060.802 | 0.664/1.471/2.249 | 52.192/65.333/71.555 | 1096.791/1127.563/1131.305 | 2.468%/19.931% | 6.062/7.324 | 0 |
| WeldAgent | 202.174 | 0.455/0.782/1.088 | 12403.929/12596.438/12599.910 | 0.864/1.669/1.746 | 24.979/37.036/37.488 | 12437.966/12621.767/12627.529 | 7.128%/92.078% | 7.324/47.329 | 0 |

PTV2 记录到 inference `33.658 ms`、total `792.673 ms`；WeldAgent 不提供对应字段。
该基准不代表 PTV2/WeldAgent 性能上限，也不是 GPU/CUDA、并发 Job、压力或 soak test；
两个业务仍完全独立。

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
