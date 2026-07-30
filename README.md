# IndustrialAIServiceFramework

面向工业 AI 应用的 C++ 高性能任务服务框架。

> 当前状态：Phase 2 Linux Reactor Core 已实现，Windows Visual Studio 2022 Debug/Release 基础层回归均为 43/43；新增 Linux Reactor 测试尚未在真实 Linux CI 中运行。当前状态标记为 `PHASE_2_REACTOR_CORE_IMPLEMENTED_LINUX_VALIDATION_BLOCKED`，不能写成 Phase 2 completed。

## 项目定位

项目最终目标是在 Linux 上使用 C++17、POSIX Socket、epoll ET、单 Reactor、线程池和插件机制，把工业算法封装为可提交、可查询、可超时的任务服务。

框架层只负责网络、协议、路由、任务调度、插件管理、状态、日志、配置和错误处理。焊缝、点云、机器人等领域语义只能进入插件层。

Phase 1 只建立可测试的公共基础设施，不启动网络服务，也不进入常驻循环。

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
- 注册期内 Channel 地址必须稳定，fd 必须有效；析构前必须移除。active 批次中的 Channel 必须活到整批分派结束，移除和销毁通过 `queue_in_loop` 延迟。
- `queue_in_loop` 在同一互斥区内完成状态/容量检查、入队、eventfd 唤醒和失败回滚；Stopping/Stopped 拒绝新回调。
- Created 允许预先入队；run 前 stop 会直接进入 Stopped 并取消尚未执行的回调。Running stop 进入 Stopping，唤醒 epoll，处理已接受回调后进入 Stopped。
- 一个 Channel 回调抛异常时，该 Channel 本次剩余回调停止；EventLoop 记录异常并继续后续 active Channel。pending callback 异常同样不会终止循环。

## 尚未实现

- Acceptor、TcpConnection、TcpServer、bind/listen/accept 和网络监听
- HTTP request/response/parser/router
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

Phase 2 (implemented, Linux validation blocked)
  UniqueFd / Socket / Channel / EpollPoller / EventLoop / eventfd

Phase 3—7 (planned)
  HTTP -> task system -> static plugins -> timers -> async logging

Phase 8—9 (planned)
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

`IAISF_BUILD_LINUX_NETWORK=ON` 只允许在 Linux 使用。非 Linux 平台显式开启会在 CMake configure 阶段报错；Windows 默认关闭并继续只构建 `iaisf_core`、`iaisf_server` 和 Phase 1 测试。

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

脚本会显式传入 `-DIAISF_BUILD_LINUX_NETWORK=ON`。Phase 1 的 [Linux CI run 30508113122](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122) 已成功；Phase 2 当前改动尚未 commit/push，因此没有对应的真实 Linux run。完整状态见 [stage_status.md](docs/stage_status.md)，构建说明见 [linux_build.md](docs/linux_build.md)。

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

它没有后台线程、异步队列、文件 sink、轮转或压缩。这些属于 Phase 7。

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

真实结果见 [stage_status.md](docs/stage_status.md)。本轮 Windows/MSVC Debug/Release 均为 43/43；Phase 2 当前有 44 个 Linux-only 测试定义，若全部被发现则 Linux CTest 预计接近 87 项，但尚未在目标平台运行，不能把定义数或预计数写成 PASS。

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
│   ├── logging/
│   ├── net/
│   └── version.hpp.in
├── src/
│   ├── app/
│   ├── config/
│   ├── core/
│   ├── logging/
│   ├── net/
│   └── main.cpp
├── tests/
│   └── net/
├── scripts/
└── docs/
```

`net` 只包含 Phase 2 Reactor 原语，没有 Acceptor、TcpConnection、HTTP、任务或插件空壳类。

## 与 TinyWebServer 的差异

`TinyWebServer_reference` 仅用于只读了解 Socket、epoll、HTTP、线程池、定时器和日志等通用思想。本项目没有复制其源码、类名或目录结构。

本项目采用分层模块、RAII、实例依赖注入、统一 Result、任务状态机和插件边界，不包含 MySQL 登录、HTML、静态文件、CGI 或参考工程性能数字。

## 性能

未执行性能测试，没有 QPS、并发连接数、延迟、CPU 或内存结论。性能只能在 Phase 8 按真实硬件和原始命令测量后填写。

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
