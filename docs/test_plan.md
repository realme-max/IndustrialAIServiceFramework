# 测试计划

## 1. 状态与原则

Phase 1 已实现基础单元测试和 CLI smoke，并在 Phase 1B 加强 Error 边界、Result 引用类别、配置数值类型和 UTF-8 字节限制覆盖。Phase 2 Reactor 实现提交为 `f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f`，warning 修复及最终验证提交为 `4db8708a5121f8477d835addd0b16170a3e2054f`；[GitHub Actions Linux CI run 30516007475](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475) 已完成 Debug、Release、CTest 和 Release smoke 零 warning 验证，当前状态为 `PHASE_2_REACTOR_CORE_COMPLETED`。

测试原则：

- 自动运行，不依赖手工点击。
- 不依赖 GPU、真实点云、机器人或外部数据库。
- 单元测试优先使用确定性输入、可注入时钟和本地 socket。
- 集成测试启动真实服务进程，动态选择空闲端口并可靠回收。
- 性能数字只在 Phase 8 实际测量后记录。
- 测试失败必须保留原始命令和足够诊断，不以重试掩盖竞态。

## 2. 测试层次

```mermaid
flowchart TB
    Unit["Unit<br/>value objects, parsers, state machines"]
    Component["Component<br/>epoll loop, thread pool, task/plugin/timer"]
    Integration["Integration<br/>real server + HTTP/TCP client"]
    Sanitizer["Sanitizers<br/>ASan, UBSan, optional TSan"]
    Performance["Performance<br/>wrk/ab and resource measurements"]

    Unit --> Component
    Component --> Integration
    Integration --> Sanitizer
    Sanitizer --> Performance
```

Phase 1—7 的 CI 门禁以前三层和适用 sanitizer 为主；性能测试不作为不稳定的每次提交单元门禁。

## 3. 工具与组织

计划：

- GoogleTest/GoogleMock：单元和组件测试。
- CTest：统一发现、标签、超时和退出码。
- shell/Python 标准库可用于集成测试驱动，但核心服务端必须是 C++；优先使用项目内 C++ client 或 `curl`。
- ASan：内存越界、use-after-free、泄漏。
- UBSan：未定义行为。
- TSan：可用 Linux/Clang 环境下检查仓储、线程池和 logger；与 ASan 分开运行。
- Valgrind：可选慢速补充，不替代 sanitizer。
- wrk 或 ab：Phase 8 HTTP 压测。

推荐测试命名：

```text
tests/unit/<module>_<subject>_test.cpp
tests/integration/<scenario>_test.cpp
```

CTest labels：`unit`、`integration`、`linux`、`sanitizer`、`slow`。

## 4. Phase 1 测试

| 测试 | 目标 |
|---|---|
| `VersionTest` | 版本常量格式和值一致 |
| `ErrorTest` / `ResultTest` | 稳定错误码、空消息边界、value/error 引用类别、void、move-only 和 API 误用 |
| `AppConfigTest` | 默认值、真实示例、类型/范围、负数、float/string/null/bool、UTF-8 bytes、未知字段和错误分类 |
| `LogLevelTest` / `ConsoleLoggerTest` | 级别解析、阈值、格式、换行和字段清洗 |
| `ApplicationTest` | help/version/config/无参数/非法参数/错误不抛出 |
| `iaisf_server_version` | CTest 固定 CLI 版本输出 |
| `iaisf_server_example_config` | CTest 使用源码树真实示例配置的 CLI smoke |
| Out-of-source check | 源码目录无生成物 |

正式 Linux 命令：

```bash
cmake -S . -B build/linux-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIAISF_BUILD_TESTS=ON \
  -DIAISF_USE_SYSTEM_DEPS=OFF
cmake --build build/linux-debug --parallel
ctest --test-dir build/linux-debug --output-on-failure

cmake -S . -B build/linux-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DIAISF_BUILD_TESTS=ON \
  -DIAISF_USE_SYSTEM_DEPS=OFF
cmake --build build/linux-release --parallel
ctest --test-dir build/linux-release --output-on-failure
./scripts/smoke_linux.sh
```

GitHub Actions 证据：

| 项目 | 实际记录 |
|---|---|
| workflow | `Linux CI` |
| run | [30508113122](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122) |
| event / attempt | `push` / `1` |
| commit | `63b30cffcbe3e621af33664721b3675a647bd1a1` |
| branch | `phase/1-foundation` |
| final status / conclusion | `completed` / `success` |
| runner | 两个 job 均为 GitHub-hosted `ubuntu-24.04` |
| OS | Debug 环境记录为 Ubuntu 24.04.4 LTS |
| compiler | Debug 与 Release configure 均识别 GNU 13.3.0 |
| CMake | Debug 环境记录为 3.31.6；Release job 未单独重复打印版本 |

真实测试矩阵：

| 环境/job | Configure | Build | CTest 总数/通过/失败 | Smoke | 项目编译 warning |
|---|---|---|---|---|---:|
| GitHub Actions Linux Debug | pass | pass | 43 / 43 / 0 | 不适用 | 0 |
| GitHub Actions Linux Release | pass | pass | 43 / 43 / 0 | pass | 0 |
| Windows VS2022 Debug（补充） | pass | pass | 43 / 43 / 0 | CTest 内 2 项 pass | 0 |
| Windows VS2022 Release（补充） | pass | pass | 43 / 43 / 0 | version/config 手工执行 exit 0 | 0 |

Release smoke 原始输出摘要：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T02:19:47.674Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

`Smoke Release` 步骤、两个 build 步骤和两个 CTest 步骤均为 `success`；完整 workflow conclusion 为 `success`，没有用 cancelled、skipped、neutral 或单步重跑代替完整成功运行。Linux 编译日志没有匹配到项目 warning。

Phase 1B 的临时配置 fixture 使用原子创建的唯一系统临时目录，不仅依赖进程内序号；因此多个 CTest 进程并行执行时不会共享配置路径，析构时通过 RAII 清理整个测试目录。

## 5. Phase 2 Network/Reactor 基础测试

状态：completed。Phase 2 只验证 fd RAII、Linux Socket 基础封装和事件循环原语。

### 5.1 Unit/component

| 测试文件 | 定义数 | 覆盖 |
|---|---:|---|
| `tests/net/test_unique_fd.cpp` | 8 | 默认/显式有效性、fd 0、析构、release/reset、move construct/assign、swap、禁止复制 |
| `tests/net/test_socket.cpp` | 5 | 创建 IPv4 TCP、`O_NONBLOCK`、`FD_CLOEXEC`、`SO_REUSEADDR`、所有权转移与 write-half shutdown |
| `tests/net/test_channel.cpp` | 7 | mask；RDHUP read；HUP-only close；HUP+IN read；ERR→read 与 read→write 顺序；非 owning |
| `tests/net/test_epoll_poller.cpp` | 7 | add/mod/del、重复/未注册操作、eventfd ET、容量/timeout、fd 删除后复用映射 |
| `tests/net/test_event_loop.cpp` | 17 | owner/state、stop-before-run、Stopping 拒绝、跨线程容量竞争、失败回调不执行、唤醒失败回滚、嵌套 queue、active 批次延迟移除、异常边界、eventfd drain/EAGAIN |

合计 44 个 Phase 2 Reactor `TEST`。Linux Debug 和 Release 均实际发现并执行全部 44 项；加上 Phase 1 的 43 项，每个配置实际执行 87 项。

正式 Linux 验证矩阵：

| 环境/job | Configure | Build | CTest 总数/通过/失败 | Reactor 通过 | Smoke | warning |
|---|---|---|---|---|---|---:|
| Ubuntu 24.04 GCC Debug | pass | pass | 87 / 87 / 0 | 44 / 44 | CTest 内 2 项 pass | 0 |
| Ubuntu 24.04 GCC Release | pass | pass | 87 / 87 / 0 | 44 / 44 | 独立 version/config pass | 0 |
| Windows VS2022 Debug（网络 OFF） | pass | pass | 43 / 43 / 0 | 不适用 | CTest 内 2 项 pass | 0 |
| Windows VS2022 Release（网络 OFF） | pass | pass | 43 / 43 / 0 | 不适用 | version/config exit 0 | 0 |

首次功能 CI：

- [run 30514521602](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30514521602) 对应 Reactor 实现提交 `f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f`。
- Debug/Release configure、build、87/87 CTest 和 smoke 均通过。
- Release 测试构建存在 2 条 `-Wunused-result`，来自 `tests/net/test_event_loop.cpp:721` 和 `:746` 忽略 `read()` 返回值；该 run 不是最终零 warning 证据。

最终零 warning CI 证据：

| 项目 | 实际记录 |
|---|---|
| workflow / run | `Linux CI` / `30516007475` |
| URL | [https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475) |
| event / attempt | `push` / `1` |
| branch / head SHA | `phase/2-reactor-core` / `4db8708a5121f8477d835addd0b16170a3e2054f` |
| checkout SHA | `4db8708a5121f8477d835addd0b16170a3e2054f`，Debug/Release 一致 |
| status / conclusion | `completed` / `success` |
| runner / OS | GitHub-hosted `ubuntu-24.04` / Ubuntu 24.04.4 LTS |
| GCC / CMake | 13.3.0 / 3.31.6 |
| jobs | `Linux Debug`、`Linux Release` 均为 `success` |
| CTest | Debug 87/87、Release 87/87，均为 0 failed |
| Reactor | Debug 44/44、Release 44/44 |
| targets | `iaisf_net`、`iaisf_net_tests` 在 Debug/Release 均实际构建 |
| warning | 项目源码 0；项目测试 0 |
| 非成功步骤 | 0；没有 skipped、cancelled、neutral |
| `continue-on-error` | 对应 workflow revision 未配置；日志无被掩盖的非零退出 |

Release smoke 原始输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T05:14:04.588Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

最终编译日志核验：Debug/Release 项目源码 warning 为 0，项目测试 warning 为 0，
两处 `-Wunused-result` 已消失。checkout 日志中的 `git init` 默认分支提示含有单词
“warning”，但不是项目或第三方编译 warning。

Linux 原始命令：

```bash
cmake -S . -B build/linux-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIAISF_BUILD_TESTS=ON \
  -DIAISF_USE_SYSTEM_DEPS=OFF \
  -DIAISF_BUILD_LINUX_NETWORK=ON
cmake --build build/linux-debug --parallel
ctest --test-dir build/linux-debug --output-on-failure

cmake -S . -B build/linux-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DIAISF_BUILD_TESTS=ON \
  -DIAISF_USE_SYSTEM_DEPS=OFF \
  -DIAISF_BUILD_LINUX_NETWORK=ON
cmake --build build/linux-release --parallel
ctest --test-dir build/linux-release --output-on-failure
./scripts/smoke_linux.sh
```

补充检查：

- 非 Linux 平台默认不构建 `iaisf_net`；显式 `-DIAISF_BUILD_LINUX_NETWORK=ON` 必须在 configure 阶段失败。
- Reactor 测试只使用 `eventfd` 和 `socketpair`，不监听端口、不访问外部网络。
- 等待使用 condition variable/future 的有限超时；`iaisf_net_tests` CTest timeout 为 10 秒。
- 并发容量测试用 condition variable 同步 32 个生产者，不用固定 sleep；成功数不超过容量，每个成功回调最多执行一次，失败回调执行次数为 0。
- active 批次测试验证直接 remove 被拒绝、通过 `queue_in_loop` 延迟后成功，并保证同批其他 Channel 仍被处理。
- Channel 异常测试固定策略：当前 Channel 剩余回调停止，后续 active Channel 继续；pending callback 同时覆盖标准和未知异常。
- 当前未启用 sanitizer；ASan/UBSan 仍是后续可选验证，不能写成已通过。

### 5.2 Phase 2 暂不执行

- HTTP parser/request/response/router。
- Buffer、完整 `TcpConnection` 协议处理、Acceptor/TcpServer 和 TCP echo 集成。
- ThreadPool、TaskRepository、TaskManager 和 PluginManager。
- timerfd 任务超时、signalfd 优雅停止。
- 异步日志或 AI 插件。

### 5.3 Phase 3 计划测试边界

Phase 3 尚未开始。建议自动化覆盖 Buffer、Acceptor/TcpConnection/TcpServer、
ET accept/read/write 到 `EAGAIN`、`EPOLLOUT` 动态启停、输出缓冲高水位、连接
建立/半关闭/关闭生命周期，以及原始字节 Echo 集成测试。Phase 3 测试不得提前引入
HTTP parser、HttpRouter、ThreadPool、TaskRepository、TaskManager、PluginManager、
timerfd、signalfd、异步日志、AI 推理或 benchmark。

### 5.4 fd 泄漏

Phase 2 单元测试重复创建和释放受管 fd，并检查所有权转移后只关闭一次。真实服务进程的 `/proc/<pid>/fd` 稳定性检查延后到连接层集成阶段。

## 6. HTTP Parser 测试

必须覆盖：

- 完整 GET。
- 完整 POST + JSON body。
- request line、每个 header、CRLF 分界和 body 的所有关键分段点。
- 一字节一字节输入。
- body 后紧跟下一请求，解析器保留剩余字节。
- 非完整请求返回 NeedMore，不产生 400。
- 非法 method/version/target。
- 裸 LF、错误 CRLF、header 缺冒号、非法 header name。
- 缺 Host。
- Content-Length 缺失（POST 规则）、非数字、负数、溢出、冲突重复。
- Transfer-Encoding、TE + CL、chunked。
- 请求行/header 数/header 总量/body 超限。
- 空 JSON、畸形 JSON、合法但顶层非 object。
- keep-alive 和 `Connection: close`。
- 路由 404、方法 405、路径参数提取。
- `..`、编码斜杠、NUL/反斜杠路径拒绝。

解析测试不需要真实 Socket；向 parser 多次 feed 字节片段即可。

## 7. ThreadPool 与队列测试

- 固定 worker 数启动。
- 多任务恰好执行一次。
- 有界队列满时 `submit` 返回 false。
- `submit` 与 worker 并发无数据竞争。
- condition variable 在空队列阻塞，无 busy waiting。
- stop 后拒绝新任务。
- drain stop 等待已接受任务。
- 多次 stop 幂等。
- 析构 join 所有线程，无 detached worker。
- 任务抛 `std::exception`/未知异常后 worker 继续处理后续任务。
- worker_count、queue_size 为 0 等非法构造失败。

避免依赖固定 sleep 判断并发；使用 latch/barrier 的 C++17 自定义测试辅助和 condition variable。

## 8. Task 测试

### 8.1 状态转换

允许：

- Queued → Running/Cancelled
- Running → Succeeded/Failed/Cancelled/Timeout

拒绝：

- Succeeded → Running
- Failed → Queued
- Timeout → Succeeded
- 任意终态 → 其他状态
- 重复完成（除非明确作为幂等 no-op，首版建议返回 false）
- Queued → Failed（排队失败不创建任务；执行错误必须先进入 Running）

### 8.2 并发与容量

- success 与 timeout 同时竞争，只有一个成功转换。
- cancel 与 worker start 竞争。
- 查询得到自洽快照，时间字段满足 create ≤ start ≤ finish。
- progress 范围校验。
- 队列满不留下 task。
- repository 满拒绝新任务。
- 终态清理后查询 404。
- 插件晚到结果不覆盖 Timeout。
- Succeeded、Failed 与 Timeout 同时到达时，第一个被 TaskRepository 接受的终态获胜。

## 9. Plugin 测试

- 注册 Echo，按名称获取。
- 空名/非法名拒绝。
- 重复名称拒绝且原插件不被覆盖。
- 未知/disabled 插件。
- initialize 成功、失败、抛异常。
- shutdown 顺序和幂等行为。
- Echo 原样封装输入且无共享状态。
- MockVision：
  - 有效输入产生 `mock: true`；
  - 标志不可关闭；
  - 缺路径、绝对路径、`..`、NUL、非法 hint 被拒绝；
  - 不要求文件真实存在；
  - 延迟配置边界；
  - cancellation 能在模拟延迟中尽快返回。
- execute 显式失败/抛异常被 TaskExecutor 转成 Failed，worker 存活。

核心测试不链接 PCL、TensorRT、CUDA 或机器人 SDK。

## 10. Timer 测试

TimerQueue 通过可注入单调时钟或纯堆核心测试，避免真实等待：

- add 后按 expire 顺序执行。
- 同截止时间全部执行。
- cancel 后不执行。
- cancel 不存在/已执行 timer 返回明确结果。
- update 提前/延后，只执行新 generation。
- 回调中 add/cancel 的重入策略。
- timer_id wrap/generation 边界（可模拟）。
- 空堆时 timerfd disarm。
- 到期批量执行后正确设置下一个 timerfd。
- idle connection 活动续期。
- task completion 取消 timeout。

真实 timerfd 只做少量 Linux 集成测试，并给出宽松但有限的时间窗口。

## 11. Config 测试

- 完整合法配置。
- 缺失可选字段使用文档默认。
- 缺失关键字段的既定行为。
- 错误 JSON、错误类型、负数、零、端口溢出、过大容量。
- queue/record/timeout 之间的跨字段约束。
- 未知顶层字段。
- disabled/enabled 插件配置。
- enabled 插件初始化失败导致启动失败。
- 日志目录不可创建/文件不可写。
- 配置错误输出不泄露秘密。

测试使用临时目录，不能修改真实 `config/iaisf.example.json`。

## 12. Logger 测试

- 每个级别过滤正确。
- 时间、线程 ID、level、message 行完整。
- 多 producer 无行交错。
- 有界队列溢出策略和 dropped counter。
- 批量写和定期 flush。
- stop 前 drain/flush。
- 多次 stop。
- 文件打开失败策略。
- 基础按大小轮转，文件数量/命名稳定。
- sink 写失败不递归记录或死锁。

## 13. 集成场景

必须自动化：

1. `/health` 返回 200 JSON。
2. 提交 Echo 任务，轮询到 success，结果与输入一致。
3. 提交 MockVision，结果包含 `mock: true` 和结构化模拟字段。
4. 查询 queued/running/terminal 状态。
5. 非终态结果查询返回 409。
6. 未知插件返回 404。
7. 错误 JSON 返回 400。
8. 请求 body 超限返回 413。
9. 队列满返回 503。
10. 任务超时返回 timeout/504，晚到结果不覆盖。
11. idle connection 关闭，活动连接保持。
12. keep-alive 上连续请求。
13. 多客户端并发提交和查询。
14. SIGTERM 后停止 accept、按策略 drain、刷新日志并以 0 退出。

队列满不能映射为插件错误；429 只用于未来单独的用户级限流。worker 完成路径测试必须确认它只写完成队列并通过 eventfd 唤醒 EventLoop，不直接操作 Socket、Channel 或 epoll。

测试 harness 要求：

- 动态获取端口，避免固定 8080 冲突。
- 启动后轮询 readiness，不能固定长 sleep。
- 失败时捕获 server stdout/stderr/log。
- 设置总 timeout。
- 无论成功失败都终止并 wait 服务进程。

## 14. 安全回归

- 超大 Content-Length 不导致预分配。
- 慢速分段请求受 idle/header deadline 限制（Phase 6 后）。
- 多个 Content-Length 请求走固定拒绝路径，防请求走私。
- `../`、绝对路径、反斜杠、百分号编码绕过。
- 客户端 JSON 中的 `"command"` 只被视为普通/未知字段，绝不执行。
- 日志换行和控制字符被转义，防日志注入。
- 错误响应不含本地绝对路径、errno 文本或异常原文。
- 连接/队列/日志/任务仓储容量均可触发并恢复。

## 15. Sanitizer 与静态检查

计划命令形态：

```bash
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DIAISF_ENABLE_ASAN=ON \
  -DIAISF_BUILD_TESTS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

UBSan/TSan 分开 build 目录。每次记录编译器、sanitizer 选项和被排除测试。不能只写“ASan 通过”而没有命令和日期。

## 16. Phase 8 性能与稳定性

### 16.1 工作负载

- `GET /health`：测网络/HTTP 基线。
- Echo submit + status/result：测任务框架。
- MockVision 固定 mock delay：测 worker/队列行为，不代表视觉推理性能。
- 长 keep-alive、多短连接分别测试。

### 16.2 记录模板

```text
Date:
Commit:
OS / kernel:
CPU / logical cores:
Memory:
Compiler / version:
Build type / flags:
Server config:
Workload:
Client tool / version:
Raw command:
Duration:
Concurrency:
Observed throughput:
Latency distribution:
Server CPU:
Server RSS:
Errors/timeouts:
Raw output path:
Interpretation and limitations:
```

### 16.3 禁止项

- 不沿用 TinyWebServer README 的性能数字。
- 不用 mock 延迟结果宣称真实模型吞吐。
- 不只报告平均延迟，至少保留工具给出的分位数。
- 不在 Debug/ASan 结果上包装生产性能。
- 不删除失败请求后只展示成功数字。

## 17. Phase 0 历史调查验证

Phase 0 的非运行验证项：

- 根目录和参考目录均不是 Git 仓库。
- 参考工程 62 个文件、总大小 59,240,225 bytes。
- 调查时参考聚合 SHA-256：
  `83AE7E469DEA30C860DEFD4D26CB313B7B3C87EFCD9387414741E152EE46CF27`。
- 当前宿主无可用 Linux/epoll 构建环境，因此没有伪造编译结果。
- Phase 0 结束时复算哈希应一致。
