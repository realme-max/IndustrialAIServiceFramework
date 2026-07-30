# IndustrialAIServiceFramework

面向工业 AI 应用的 C++ 高性能任务服务框架。

> 当前状态：`PHASE_4_HTTP_PROTOCOL_IMPLEMENTED_LINUX_VALIDATION_BLOCKED`。Phase 4 已实现可移植 HTTP/1.1 Core 和 Linux TCP adapter；Windows Visual Studio 2022 Debug/Release 均实际构建 `iaisf_http_core`、执行 Foundation 43 + HTTP Core 83，共 126/126 CTest 通过，项目 warning 为 0。Linux-only `HttpSession/HttpServer` 与 16 项集成测试尚未由新提交的真实 Linux CI 验证，因此不能标记 Phase 4 completed。

## 项目定位

项目最终目标是在 Linux 上使用 C++17、POSIX Socket、epoll ET、单 Reactor、线程池和插件机制，把工业算法封装为可提交、可查询、可超时的任务服务。

框架层只负责网络、协议、路由、任务调度、插件管理、状态、日志、配置和错误处理。焊缝、点云、机器人等领域语义只能进入插件层。

当前 `iaisf_server` 仍只验证 CLI 和配置后退出；Phase 4 已提供可组合的 HTTP Server API，但尚未接入常驻服务组合根，也没有新增不受控的 `--serve`。

## Phase 1 已实现

- CMake 3.22+、C++17、target 级依赖与编译警告配置
- `iaisf_core` 静态库、`iaisf_server`、`iaisf_tests`
- CMake `configure_file` 生成的稳定版本信息 `0.1.0`
- `ErrorCode`、`Error`、`Result<T>` 和 `Result<void>`
- nlohmann/json 驱动的严格 `AppConfig`
- `LogLevel`、`ILogger` 和同步、互斥保护的 `ConsoleLogger`
- 支持 `--help`、`--version`、`--config <path>` 的最小 `Application`
- GoogleTest + CTest 单元测试和 CLI smoke 测试
- Debug/Release Linux 构建、测试和 smoke 脚本
- Ubuntu 24.04 GCC Debug/Release GitHub Actions workflow
- Apache License 2.0

Phase 1B 审计修正了 Error 非空消息的文档边界，移除了未实现的 CMake 安装接口，并加强了 Result 引用类别、配置类型和 UTF-8 字节上限测试。2026-07-30 的 Windows/MSVC Debug、Release 补充回归和 Ubuntu 24.04 GCC Debug、Release CI 回归均为 43/43 CTest 通过；Linux `--version` 与示例配置 smoke 也已通过。

## Phase 2 已实现

- Linux-only `iaisf_net` 静态库和 `iaisf::net` alias
- move-only `UniqueFd`，析构只尝试一次 `close`
- 非阻塞、close-on-exec 的 IPv4 TCP `Socket` 基础封装
- 不拥有 fd 的 `Channel`、稳定事件掩码和 read/write/error/close 回调
- 有界事件数组的 `EpollPoller`
- 单线程 `EventLoop` 与 `Created/Running/Stopping/Stopped` 状态
- `eventfd(EFD_NONBLOCK | EFD_CLOEXEC)` 跨线程唤醒
- 有界待执行回调队列，满时返回 `ResourceExhausted`
- 回调异常隔离和注入式 `ILogger`
- 44 个 Linux-only Reactor 测试定义

Phase 2 没有把 Reactor 接入 `iaisf_server`；命令行程序仍只验证版本和配置后退出。

Phase 2B 固定了以下并发语义：

- `EPOLLRDHUP` 属于 read-side 通知；`EPOLLHUP` 只有在没有 read-side 事件时才直接触发 close，因此 `HUP|IN` 仍可读取剩余数据。
- Channel 只通知事件，不自动把 ET fd 读写到 `EAGAIN`；该职责属于未来连接层。
- 注册期内 Channel 地址必须稳定，fd 必须有效；析构前必须移除。active 批次中的 Channel 必须活到整批分派结束。应用短回调使用有界 `queue_in_loop`；框架生命周期清理使用独立的内嵌 intrusive 节点，不能被普通队列容量拒绝。
- `queue_in_loop` 在同一互斥区内完成状态/容量检查、入队、eventfd 唤醒和失败回滚；Stopping/Stopped 拒绝新回调。
- Created 允许预先入队；run 前 stop 会直接进入 Stopped 并取消尚未执行的回调。Running stop 进入 Stopping，唤醒 epoll，处理已接受回调后进入 Stopped。
- 一个 Channel 回调抛异常时，该 Channel 本次剩余回调停止；EventLoop 记录异常并继续后续 active Channel。pending callback 异常同样不会终止循环。

## Phase 3 已完成

- Linux-only `iaisf_tcp` 静态库与 `iaisf::tcp` alias，PUBLIC 依赖 `iaisf::net`
- 仅支持数值 IPv4 的 `Ipv4Endpoint`，含端口 0、loopback/any、`sockaddr_in` round-trip
- 初始容量与硬上限分离的二进制 `Buffer`，支持前部复用、显式 compact、溢出安全增长与 `ResourceExhausted`
- `Socket` 的 bind/listen/local endpoint、`TCP_NODELAY`、`SO_ERROR` 与 `accept4`
- owner-thread-only `Acceptor`，对监听 fd 使用 epoll ET 并 accept 到 `EAGAIN`
- shared-owned `TcpConnection`，Channel 回调只捕获 weak pointer
- ET recv/send 循环、`MSG_NOSIGNAL`、部分写缓存与动态 `EPOLLOUT`
- 输入/输出 hard maximum 与仅在阈值跨越时通知的 output high-water
- peer EOF 后先交付已读数据、再排空输出并延迟移除连接
- `TcpServerOptions` 严格有符号输入和跨字段/硬上限校验，含可选的受验证
  `SO_SNDBUF` 调优值（默认不覆盖系统值）
- `TcpServer` 有界连接表、单调连接 ID、过载 RAII 拒绝和延迟销毁
- `send()` 采用 all-accepted-or-failure：首次系统发送前整包预留，failure 不写出或缓存本次前缀；可写回调中的部分内核写始终保留完整未写后缀
- 普通 pending queue 满时，Acceptor stop 和连接表清理仍由不分配的内部 deferred-cleanup lane 最终执行
- `TcpServer::stop()` owner-thread-only、幂等、永久禁止 restart；非 active 调用同步清空连接表，active 回调内调用则在批次后完成，`stopped()` 是完成屏障
- 独立 `iaisf_tcp_tests`；Linux Debug/Release 均实际发现并执行 50/50 TCP 测试

Phase 3 的线程边界不是“TCP 层整体线程安全”：只有 `EventLoop::queue_in_loop()` 和 `stop()` 可跨线程；Acceptor、TcpServer、TcpConnection、Buffer 和 Channel 的普通操作都属于 EventLoop owner 线程。

## Phase 4 已实现，等待 Linux CI

- Windows/Linux 可移植 `iaisf_http_core` / `iaisf::http_core`：`HttpStatus`、`HttpLimits`、`HttpRequest`、`HttpResponse`、增量 `HttpParser`、冻结式 `HttpRouter` 和内置路由
- Linux-only `iaisf_http` / `iaisf::http`：每连接 `HttpSession` 和组合 `TcpServer` 的 `HttpServer`
- HTTP/1.1 only、严格 CRLF、origin-form target、Content-Length only、二进制 body、默认 keep-alive 和 `Connection: close`
- 有限顺序 pipelining；每轮最多处理 `max_requests_per_dispatch`，通过普通有界 `queue_in_loop` 继续，入队失败即关闭该连接
- 请求行与 header line 上限包含结尾 CRLF；header total/count、request body、response head/body、route count 全部有硬上限
- 所有重复 header name（ASCII 大小写不敏感）均 fail-closed；Host 必须恰好一个；CL+TE、歧义长度和 obs-fold 拒绝；Transfer-Encoding/chunked 返回 501，Expect 返回 417
- `Connection` 只按 comma-separated 的完整、大小写不敏感 token 识别 `close`，相似子串不匹配，空 token 或非法 token 拒绝
- `HttpResponse` 自动生成 Content-Length/Connection，拒绝 framing header 覆盖和 CRLF 注入；响应 header count、单行和 head total 预检包含自动 framing 行，失败不返回部分响应
- 精确 method+path 路由、稳定 404/405 + Allow、handler Error/异常隔离为关闭连接的 500
- `Connection: close` 使用写尽后全关闭，不等待客户端先发 EOF；`shutdown()` 保留为独立的半关闭契约
- 每个 Session 同时至多一个 continuation；弱引用和连接状态检查避免停服/断连后再次 dispatch；Session 移除沿用不受普通 pending queue 容量影响的 `DeferredCleanup`
- 显式注册 `GET /health`（只表示 HTTP/EventLoop 可响应）与 `GET /version`
- 83 项可移植 HTTP Core 测试已在 Windows Debug/Release 实际通过；16 项 Linux loopback/port-0 集成测试已定义，等待真实 CI

不支持 HTTP/1.0、HTTP/2、absolute/authority/asterisk-form、chunked、trailers、Upgrade、Expect、percent-decoding、路径规范化、动态路由、流式 body、TLS 或 WebSocket。

## 尚未实现

- 线程池和有界工作队列
- Task、TaskManager、TaskRepository 和任务 API
- PluginManager、EchoPlugin、MockVisionPlugin 或动态 `.so`
- timerfd、任务超时和连接超时
- 异步日志、文件日志和日志轮转
- TensorRT、PCL、真实点云、机器人或 Agent 能力
- 性能压测和任何 QPS/延迟结论

## 架构 Roadmap

```text
Phase 1 (completed)
  C++17 / CMake / Error / Result / AppConfig / ConsoleLogger / CLI / tests

Phase 2 (completed)
  UniqueFd / Socket / Channel / EpollPoller / EventLoop / eventfd

Phase 3 (completed)
  IPv4 endpoint / bounded Buffer / Acceptor / TcpConnection / TcpServer

Phase 4 (implemented; Linux validation blocked)
  HTTP/1.1 core / Router / Session / HttpServer / built-in health and version

Later phases (planned)
  task system -> static plugins -> timers -> async logging

Phase 9—10 (planned)
  measured engineering baseline -> production vision-plugin boundary
```

目标架构和阶段边界见 [architecture.md](docs/architecture.md) 与 [development_plan.md](docs/development_plan.md)。

## 依赖

固定版本：

- nlohmann/json `v3.11.3`
- GoogleTest `v1.15.2`

默认使用 FetchContent，源码和构建产物只进入 build 目录：

```text
IAISF_BUILD_TESTS=ON
IAISF_USE_SYSTEM_DEPS=OFF
IAISF_BUILD_LINUX_NETWORK=ON  # Linux 默认；非 Linux 默认 OFF
```

启用 `IAISF_USE_SYSTEM_DEPS=ON` 时使用 `find_package`，缺少兼容依赖会明确失败，不会静默切换模式。关闭 `IAISF_BUILD_TESTS` 后不获取 GoogleTest。

`IAISF_BUILD_LINUX_NETWORK=ON` 只允许在 Linux 使用。非 Linux 平台显式开启会在 CMake configure 阶段报错；Windows 默认关闭，仍会构建可移植的 `iaisf_http_core` 和 `iaisf_http_core_tests`，但不会解析 `HttpSession/HttpServer`、Reactor 或 TCP Linux 源码。

## Linux 构建

需要：

- Linux 或 WSL2 Linux
- GCC/Clang 的 C++17 工具链
- CMake 3.22+
- Git 和 CA 证书（默认 FetchContent 首次下载需要）

Debug：

```bash
./scripts/build_linux.sh Debug
./scripts/test_linux.sh Debug
```

Release：

```bash
./scripts/build_linux.sh Release
./scripts/test_linux.sh Release
```

Smoke：

```bash
./scripts/smoke_linux.sh
```

脚本会显式传入 `-DIAISF_BUILD_LINUX_NETWORK=ON`。首次功能 [Linux CI run 30514521602](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30514521602) 对应 Reactor 实现提交 `f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f`，功能和测试通过，但 Release 测试构建存在 2 条 `-Wunused-result` warning。提交 `4db8708a5121f8477d835addd0b16170a3e2054f` 修复这两处返回值检查；最终 [Linux CI run 30516007475](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475) 在 Ubuntu 24.04.4 LTS、GCC 13.3.0、CMake 3.31.6 上完成 Debug/Release configure、build 和 87/87 CTest，两个配置均实际执行 44/44 Reactor 测试，Release 两项 CLI smoke 成功，项目源码和测试 warning 均为 0。两个 job 和所有步骤均成功，没有 failed、cancelled、skipped、neutral 或 `continue-on-error`。完整状态见 [stage_status.md](docs/stage_status.md)，构建说明见 [linux_build.md](docs/linux_build.md)。

Phase 3 最终 [Linux CI run 30524686201](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30524686201) 在两个 job 中实际构建 `iaisf_tcp` 和 `iaisf_tcp_tests`，随后执行完整 CTest；Debug/Release 均为 138/138，通过数按实际日志核对为 Foundation 43、Reactor 45、TCP 50。
Phase 4 workflow 已增加对 `iaisf_http_core`、`iaisf_http_core_tests`、`iaisf_http`、`iaisf_http_tests` 的显式构建；当前尚无对应新提交的真实 run URL/结果，不能沿用 Phase 3 run 作为 HTTP 验证。

## 命令行

版本：

```bash
build/linux-release/iaisf_server --version
```

稳定输出：

```text
IndustrialAIServiceFramework 0.1.0
```

帮助：

```bash
build/linux-release/iaisf_server --help
```

验证配置：

```bash
build/linux-release/iaisf_server \
  --config config/iaisf.example.json
```

无参数时输出版本和 Phase 1 提示后立即退出。未知参数、参数冲突和缺失配置路径返回非零。

## 配置

[iaisf.example.json](config/iaisf.example.json)：

```json
{
  "service": {
    "name": "IndustrialAIServiceFramework"
  },
  "runtime": {
    "worker_threads": 4,
    "task_queue_capacity": 1024
  },
  "logging": {
    "level": "info"
  }
}
```

安全默认值：

- `service.name = IndustrialAIServiceFramework`
- `runtime.worker_threads = clamp(hardware_concurrency, 1, 256)`
- `runtime.task_queue_capacity = 1024`
- `logging.level = info`

限制：

- service name 最大 128 bytes，不能空、全空白或包含控制字符；
- worker threads 范围 1—256；
- task queue capacity 范围 1—1,000,000；
- 日志级别严格区分大小写，只接受 `trace/debug/info/warn/error`；
- 分组或字段缺失使用默认值；
- 未知顶层/分组字段、错误类型和非法值返回 `ConfigError`；
- 文件不存在、无法打开或无法读取返回 `IoError`。

Phase 1 不支持 YAML、TOML、环境变量覆盖、热更新、多文件合并或注释 JSON。

## Error 与 Result

`ErrorCode` 当前包含 Phase 1 基础值和 Phase 2 Reactor 通用值：

```text
InvalidArgument / ConfigError / IoError / SystemError /
InvalidState / ResourceExhausted / InternalError
```

`Result<T>` 使用标准库 `std::variant`，支持 move-only 类型且不要求 `T` 默认构造。`Result<void>` 表达只返回成功/错误的操作。

预期失败通过 Result 返回；对失败 Result 调用 `value()`，或对成功 Result 调用 `error()`，会抛出 `std::logic_error`，表示程序员 API 误用，不用于普通错误流程。

`Error` 是公开字段的可变值类型，因此“message 非空”不是类型系统可永久维持的不变量。项目生产代码通过 `make_error` 创建 Error，构造边界和 `Result::failure` 边界都会把空消息归一化为 `unspecified error`。

## 同步日志

Phase 1 的 `ConsoleLogger`：

- 通过 `ILogger` 注入，不是全局单例；
- 支持五个日志级别和阈值过滤；
- 使用 mutex 保证一条记录完整输出；
- 使用注入的 `std::ostream&`，便于测试；
- 输出 UTC 时间、level、component 和 message；
- 转义换行和控制字符。

它没有后台线程、异步队列、文件 sink、轮转或压缩。这些属于 Phase 8。

## 测试

Linux 计划命令：

```bash
ctest --test-dir build/linux-debug --output-on-failure
ctest --test-dir build/linux-release --output-on-failure
```

测试覆盖：

- 版本和 ErrorCode 字符串
- Result 成功/失败、void、const、move-only 和误用
- 默认/合法/非法/未知字段配置
- 真实示例配置文件加载
- 日志解析、阈值、格式、换行和清洗
- Application 的 help/version/config/非法参数
- CTest CLI version 和 example-config smoke
- Linux `UniqueFd`、Socket、Channel、EpollPoller 和 EventLoop

真实结果见 [stage_status.md](docs/stage_status.md)。Phase 4 Windows/MSVC Debug/Release 均为 126/126：Foundation 43/43、HTTP Core 83/83；Release `--version` 与示例配置 smoke 均成功，项目 warning 为 0。当前 Linux 源码定义矩阵为 Foundation 43 + Reactor 45 + TCP 51 + HTTP Core 83 + HTTP integration 16 = 238 项，但这只是源码定义合计，必须等 Phase 4 CI 的 CTest 实际输出后才能写成通过。Phase 3 的历史封板结果仍是 TCP 50/50、合计 138/138；新增的 1 项 TCP close-after-write 回归属于本轮未验证源码，不能回写为历史 CI 结果。

## 项目结构

```text
IndustrialAIServiceFramework/
├── .gitattributes
├── .github/workflows/linux-ci.yml
├── CMakeLists.txt
├── LICENSE
├── cmake/
│   ├── CompilerOptions.cmake
│   └── Dependencies.cmake
├── config/
│   └── iaisf.example.json
├── include/iaisf/
│   ├── app/
│   ├── config/
│   ├── core/
│   ├── http/
│   ├── logging/
│   ├── net/
│   │   └── tcp/
│   └── version.hpp.in
├── src/
│   ├── app/
│   ├── config/
│   ├── core/
│   ├── http/
│   ├── logging/
│   ├── net/
│   │   └── tcp/
│   └── main.cpp
├── tests/
│   ├── http/
│   └── net/
├── scripts/
└── docs/
```

`net` 包含 Phase 2 Reactor 原语和 Phase 3 `tcp/` 字节传输层；`http` 包含 Phase 4 协议核心与 Linux adapter。仍没有任务、插件、定时器或异步日志空壳类。

## 与 TinyWebServer 的差异

`TinyWebServer_reference` 仅用于只读了解 Socket、epoll、HTTP、线程池、定时器和日志等通用思想。本项目没有复制其源码、类名或目录结构。

本项目采用分层模块、RAII、实例依赖注入、统一 Result、任务状态机和插件边界，不包含 MySQL 登录、HTML、静态文件、CGI 或参考工程性能数字。

## 性能

未执行性能测试，没有 QPS、并发连接数、延迟、CPU 或内存结论。性能只能在 Phase 9 按真实硬件和原始命令测量后填写。

## 许可证

Licensed under the Apache License 2.0. See [LICENSE](LICENSE).

第三方依赖保留各自许可证，本项目许可证不会重新授权第三方代码。

## 文档

- [总体架构](docs/architecture.md)
- [Linux 构建](docs/linux_build.md)
- [分阶段计划](docs/development_plan.md)
- [协议设计](docs/protocol.md)
- [插件设计](docs/plugin_design.md)
- [测试计划](docs/test_plan.md)
- [阶段状态](docs/stage_status.md)
- [上下文交接](docs/context_handoff.md)
