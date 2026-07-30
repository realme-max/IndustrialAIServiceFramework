# IndustrialAIServiceFramework

面向工业 AI 应用的 C++ 高性能任务服务框架。

> 当前状态：Phase 1 基础工程、Phase 1B 代码审计和跨平台验证已经完成，状态标记为 `PHASE_1_FOUNDATION_COMPLETED`。Windows Visual Studio 2022 Debug/Release 已完成补充验证；Ubuntu 24.04 GCC Debug/Release、CTest 和 Release smoke 已由 [GitHub Actions run 30508113122](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122) 在提交 `63b30cffcbe3e621af33664721b3675a647bd1a1` 上真实验证。

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

## 尚未实现

- Socket、epoll、Channel、Poller、EventLoop 和网络监听
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

Phase 2 (planned)
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
```

启用 `IAISF_USE_SYSTEM_DEPS=ON` 时使用 `find_package`，缺少兼容依赖会明确失败，不会静默切换模式。关闭 `IAISF_BUILD_TESTS` 后不获取 GoogleTest。

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

[Linux CI workflow](.github/workflows/linux-ci.yml) 已在 GitHub 托管的 `ubuntu-24.04` runner 上真实执行这些脚本。run `30508113122` 记录 Ubuntu 24.04.4 LTS、GCC 13.3.0 和 CMake 3.31.6；Debug、Release configure/build 成功，两个配置均为 43/43 CTest 通过，Release smoke 输出版本 `IndustrialAIServiceFramework 0.1.0` 并成功校验示例配置。完整证据见 [stage_status.md](docs/stage_status.md)，构建说明见 [linux_build.md](docs/linux_build.md)。

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

`ErrorCode` 当前只包含 Phase 1 必需值：

```text
InvalidArgument / ConfigError / IoError / InternalError
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

真实结果见 [stage_status.md](docs/stage_status.md)。Windows/MSVC Debug/Release 已完成补充回归；Ubuntu 24.04 GCC Debug/Release 已通过 GitHub Actions 验证。

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
│   └── version.hpp.in
├── src/
│   ├── app/
│   ├── config/
│   ├── core/
│   ├── logging/
│   └── main.cpp
├── tests/
├── scripts/
└── docs/
```

Phase 2 及以后目录当前可以为空；没有创建误导性的网络、任务或插件空壳类。

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
