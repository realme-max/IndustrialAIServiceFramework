# 上下文交接

## 1. 当前状态

- 项目：IndustrialAIServiceFramework
- 当前分支：`phase/2-reactor-core`
- Phase 0 提交：`5fbcec0 docs: complete phase 0 architecture design`
- Phase 1 最终实现提交：`63b30cffcbe3e621af33664721b3675a647bd1a1`
- Phase 2 起始 HEAD / main / origin/main：`6065d91b277c07ed04e64b3f08034788965e6ac1`
- 当前阶段：Phase 2 Linux Reactor Core
- 状态：`PHASE_2_REACTOR_CORE_IMPLEMENTED_LINUX_VALIDATION_BLOCKED`
- 日期：2026-07-30
- Phase 1 GitHub Actions run：[30508113122](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122)
- Phase 2 GitHub Actions run：尚无
- Phase 2 commit/push/PR：均未执行
- 阻塞：当前机器无 Linux/WSL，Phase 2 尚无真实 Linux configure/build/CTest 证据

Phase 1 已完成并保留真实 Ubuntu CI 证据。Phase 2 Reactor 原语、测试和构建接入已实现，Windows/MSVC Phase 1 回归通过；旧的 Phase 1 run 不能证明当前未提交的 Phase 2 代码，因此当前不得标记 completed。

## 2. Phase 1 文件

### 新建

```text
CMakeLists.txt
LICENSE
.gitattributes
.github/workflows/linux-ci.yml
cmake/CompilerOptions.cmake
cmake/Dependencies.cmake
config/iaisf.example.json
docs/linux_build.md
include/iaisf/version.hpp.in
include/iaisf/app/application.hpp
include/iaisf/config/app_config.hpp
include/iaisf/core/error.hpp
include/iaisf/core/result.hpp
include/iaisf/logging/log_level.hpp
include/iaisf/logging/logger.hpp
include/iaisf/logging/console_logger.hpp
src/main.cpp
src/app/application.cpp
src/config/app_config.cpp
src/core/error.cpp
src/logging/log_level.cpp
src/logging/console_logger.cpp
tests/CMakeLists.txt
tests/test_error_result.cpp
tests/test_app_config.cpp
tests/test_console_logger.cpp
tests/test_application.cpp
scripts/build_linux.sh
scripts/test_linux.sh
scripts/smoke_linux.sh
```

### 修改

```text
.gitignore
README.md
docs/architecture.md
docs/development_plan.md
docs/plugin_design.md
docs/protocol.md
docs/test_plan.md
docs/stage_status.md
docs/context_handoff.md
```

`docs/plugin_design.md` 只统一了 planned 终态命名，没有插件实现。

## 3. Phase 2 文件

### 新建

```text
include/iaisf/net/unique_fd.hpp
include/iaisf/net/socket.hpp
include/iaisf/net/channel.hpp
include/iaisf/net/epoll_poller.hpp
include/iaisf/net/event_loop.hpp
src/net/system_error.hpp
src/net/socket.cpp
src/net/channel.cpp
src/net/epoll_poller.cpp
src/net/event_loop.cpp
tests/net/test_unique_fd.cpp
tests/net/test_socket.cpp
tests/net/test_channel.cpp
tests/net/test_epoll_poller.cpp
tests/net/test_event_loop.cpp
```

### 修改

```text
.github/workflows/linux-ci.yml
CMakeLists.txt
README.md
docs/architecture.md
docs/development_plan.md
docs/test_plan.md
docs/stage_status.md
docs/context_handoff.md
docs/linux_build.md
include/iaisf/core/error.hpp
scripts/build_linux.sh
src/core/error.cpp
tests/CMakeLists.txt
tests/test_error_result.cpp
```

没有创建 Acceptor、TcpConnection、TcpServer、HTTP、ThreadPool、Task、Plugin、timerfd/signalfd 或异步日志代码。

## 4. CMake

版本和标准：

```text
CMake minimum: 3.22
C++: 17
CMAKE_CXX_EXTENSIONS: OFF
project version: 0.1.0
```

Targets：

```text
iaisf_core (STATIC)
  PRIVATE -> nlohmann_json::nlohmann_json

iaisf_server
  PRIVATE -> iaisf::core

iaisf_tests
  PRIVATE -> iaisf::core
  PRIVATE -> GTest::gtest
  PRIVATE -> GTest::gtest_main

iaisf_net (STATIC, Linux only)
  PUBLIC -> iaisf::core
  PRIVATE -> Threads::Threads

iaisf_net_tests (Linux only)
  PRIVATE -> iaisf::net
  PRIVATE -> GTest::gtest
  PRIVATE -> GTest::gtest_main
  PRIVATE -> Threads::Threads
```

`iaisf_core` 和消费者只通过 target 级 include/link/features/options 配置。GCC/Clang 项目 targets 使用：

```text
-Wall -Wextra -Wpedantic
-Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor
```

第三方 targets 不应用项目 warning。

当前没有 install/export 规则，因此 Phase 1B 已移除未落地的 `INSTALL_INTERFACE`，只保留两个 build-tree include 根目录。

`configure_file` 从 `include/iaisf/version.hpp.in` 生成：

```text
<build>/generated/include/iaisf/version.hpp
```

版本不含构建时间、机器、用户、路径、随机数或 Git hash。

`IAISF_BUILD_LINUX_NETWORK` 在 Linux 默认 ON，在其他平台默认 OFF；非 Linux 显式设为 ON 时 configure 失败。Linux 脚本显式传入 ON，Windows 回归显式传入 OFF。

## 5. 依赖

默认：

```text
IAISF_BUILD_TESTS=ON
IAISF_USE_SYSTEM_DEPS=OFF
IAISF_BUILD_LINUX_NETWORK=ON  # Linux；非 Linux 默认 OFF
```

固定 FetchContent tag：

- nlohmann/json `v3.11.3`
- GoogleTest `v1.15.2`

系统模式：

```text
IAISF_USE_SYSTEM_DEPS=ON
```

此时 `find_package` 必须找到兼容版本，否则配置失败。关闭测试时不下载或查找 GoogleTest。

## 6. Error/Result 约定

`ErrorCode` 当前稳定值：

- InvalidArgument → `invalid_argument`
- ConfigError → `config_error`
- IoError → `io_error`
- SystemError → `system_error`
- InvalidState → `invalid_state`
- ResourceExhausted → `resource_exhausted`
- InternalError → `internal_error`
- 未知枚举 → `unknown_error`

`Error` 保留公开的 code/message 值字段，调用者可以在构造后清空 message，因此非空不是类型系统永久维护的不变量。项目生产路径统一通过 `make_error` 创建；构造器、工厂和 Result failure 边界会把空 message 归一化为 `unspecified error`。

`Result<T>`：

- 使用 `std::variant` 包装 value/error；
- 支持 move-only，例如 `std::unique_ptr<int>`；
- 不要求 `T` 默认构造；
- 不允许 reference 或 void（void 使用特化）；
- 提供 success/failure、has_value、bool、value/error 和 const/rvalue access。
- 编译期断言固定 value/error 的 `&`、`const&` 和 `&&` 返回类别。

`Result<void>`：

- `success()`；
- `failure(Error)`；
- `value()` 只验证成功状态；
- error access。

误用：

- failed Result 调 `value()`；
- successful Result 调 `error()`；

统一抛 `std::logic_error`。这只表示程序员错误，预期配置和参数失败仍通过 Result/退出码处理。

## 7. AppConfig

JSON：

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

字段：

| 字段 | 默认 | 规则 |
|---|---|---|
| service.name | IndustrialAIServiceFramework | 1—128 bytes，非全空白，无控制字符 |
| runtime.worker_threads | clamp(hardware concurrency, 1, 256) | 整数 1—256 |
| runtime.task_queue_capacity | 1024 | 整数 1—1,000,000 |
| logging.level | info | 严格小写 trace/debug/info/warn/error |

缺失分组或字段使用默认值。未知顶层字段和 service/runtime/logging 内未知字段全部返回 ConfigError。

错误分类：

- 文件不存在、非普通文件、无法检查/打开/读取 → IoError；
- JSON 语法、根/组/字段类型、范围、未知字段 → ConfigError；
- `parse_log_level` 单独解析非法字符串 → InvalidArgument，由 config 层转换为 ConfigError。

不支持环境变量、YAML、TOML、注释 JSON、热更新、多文件合并或静默修正。

## 8. Logger

`ILogger`：

```text
log(level, component, message)
```

`ConsoleLogger`：

- 同步；
- 注入且不拥有 `std::ostream&`；
- 注入流必须比 logger 生命周期长，且不得在 logger 外并发直写；
- mutex 保护阈值和整行写出；
- 阈值可读取/更新；
- UTC 毫秒时间戳；
- 输出 `[LEVEL] [component] message`；
- 对换行、制表和控制字符进行单行清洗。

明确没有：

- 全局单例；
- 后台线程；
- 异步队列；
- 文件输出；
- 轮转、压缩或 JSON 日志。

## 9. Application

`Application::run` 接收不含 executable name 的 `vector<string>`，输出流可注入。

行为：

| 输入 | 行为 | 退出码 |
|---|---|---:|
| `--version` | 固定版本，不加载配置 | 0 |
| `--help` | usage，不加载配置 | 0 |
| `--config <path>` | 加载、校验、同步日志、退出 | 0/1 |
| 无参数 | 版本 + Phase 1 非网络提示 | 0 |
| 未知/冲突/缺参数/多余参数 | error + usage | 2 |
| 不可预期 main 异常 | 简洁 internal error | 70 |

`main.cpp` 不含 JSON、配置规则、日志格式、网络或业务逻辑。

## 10. Phase 2 Reactor 契约

### 所有权与生命周期

- `UniqueFd` 独占 fd；`release()` 转移所有权，`reset()` 先关闭旧 fd；析构不抛异常，也不重试 `close(EINTR)`。
- `Socket` 拥有一个 `UniqueFd`，可以整体释放/接管；本阶段只创建基础 TCP fd，不执行 bind/listen/accept/connect。
- `Channel` 不拥有 fd，也不拥有 EventLoop；Channel 地址和 fd 必须在 epoll 注册期保持有效，析构前必须成功移除。Debug 析构断言用于暴露注册期销毁。
- `EpollPoller` 拥有 epoll fd，但不拥有注册的 Channel。
- `EventLoop` 拥有 Poller 和 eventfd wakeup Channel；注入的 `ILogger` 不归 EventLoop 所有，必须活得更久。
- active event vector 中的所有 Channel 必须活到整批分派结束。EventLoop 在分派期间拒绝直接 remove；回调使用 `queue_in_loop` 延迟移除和销毁。

### 线程安全与状态

- EventLoop 构造线程是 owner；`run/update_channel/remove_channel` 仅 owner 可调用。
- `queue_in_loop` 和 `stop` 可从任意线程调用，通过 mutex 与 eventfd 协调。
- Created 可接受预提交；`Created -> Running -> Stopping -> Stopped` 是正常运行路径。run 前 stop 采用 `Created -> Stopped` 快捷路径并取消已排队回调；停止后不能重跑。
- Stopping/Stopped 不接受新回调；stop 幂等。EventLoop 必须在 owner 线程、且不处于 Running/Stopping 时析构。
- 回调队列有固定容量，交换到局部队列后再执行，允许回调嵌套提交且不在持锁时调用用户代码。
- 状态/容量检查、入队、eventfd 写入和失败回滚由同一 mutex 串行化；返回 failure 时本次回调不可能留在队列。
- Channel 分派为：HUP-only close → error → read-side（IN/PRI/RDHUP）→ write。HUP 与 read-side 共存时不直接 close；Channel 不自动 drain ET fd。
- 一个 Channel 回调抛出后，该 Channel 本轮剩余回调停止；EventLoop 记录后继续后续 active Channel。pending callback 逐个隔离；logger 抛出时增加可观察计数。

### epoll/eventfd

- epoll 使用 edge-triggered `EPOLLET`，不使用 `EPOLLONESHOT`。
- `EpollPoller` 的事件缓冲区创建时固定容量，拒绝 0 或超过 65,536 的配置。
- 重复 add、未注册 update/remove、非法 timeout 或错误 fd 返回稳定 `Result` 错误。
- eventfd 使用 nonblocking + close-on-exec；写侧将 `EAGAIN` 视为已有唤醒，读侧循环到 `EAGAIN`。
- 当前没有连接读写代码；未来 ET Socket handler 必须循环读/写直到 `EAGAIN`。

## 11. 测试

Phase 1 CTest 总数：43（41 项 unit，2 项 smoke）。

覆盖：

- 版本、ErrorCode、Error；
- Result int/string/void/failure/const/rvalue/move-only/API misuse；
- 默认/真实示例/完整/部分/非法/未知字段配置；
- 负整数不会先转成 unsigned；
- worker_threads/task_queue_capacity 明确拒绝 float、string、null 和 boolean；
- service.name 的 128 上限按解析后的 UTF-8 bytes 计算；
- LogLevel、五种阈值、格式、分行和字段清洗；
- Application 的所有命令行分支；
- CLI version/config 两个 CTest smoke。

测试不访问网络，不修改示例配置，不依赖时区固定值或当前时间精确值。Phase 1B 将临时配置改为原子创建的唯一系统临时目录，支持并行 CTest 进程且由 RAII `remove_all` 清理。

Phase 2 当前有 44 个 Linux-only `TEST` 定义：

```text
UniqueFd: 8
Socket: 5
Channel: 7
EpollPoller: 7
EventLoop: 17
```

Phase 2B 增加/加强了 RDHUP/HUP 分派、fd 复用、状态矩阵、Stopping 拒绝、容量竞争、失败回调不执行、唤醒失败回滚、active 批次延迟移除、回调异常边界、eventfd drain 和饱和计数器 EAGAIN。测试使用 eventfd/socketpair，不监听端口；并发等待均有 condition variable/future 的有限超时，CTest timeout 为 10 秒。连同 Phase 1 的 43 项，若全部发现预计接近 87 项；44 和 87 都不是 Linux PASS 数。

## 12. 环境和实际结果

Windows：

```text
Windows 10 10.0.19045 x64
MSVC 19.38.33130.0 (Visual Studio 2022)
CMake/CTest 4.1.0
Git 2.53.0.windows.1
```

Linux/WSL：

```text
wsl --status: exit 50
wsl --list --verbose: exit 1
no runnable distribution
Docker/Podman not found
```

Windows Phase 2 补充回归（网络 target 关闭，2026-07-30）：

```text
Debug configure: pass
Debug clean build: pass
Debug CTest: 43/43 pass
Release clean build: pass
Release CTest: 43/43 pass
Release --version: exit 0
Release --config example: exit 0
```

最终项目自身 MSVC warning 为 0。Visual Studio 的本机 vcpkg applocal 集成会尝试调用缺失的 `pwsh.exe` 并打印非致命诊断，构建仍为 exit 0；项目本身没有启用系统依赖模式。Windows 结果只证明 Phase 1 可移植基线未回归，不能编译或验证 Linux Reactor。

Windows 首次配置使用默认 FetchContent 模式并访问 GitHub；固定依赖获取成功，第三方源码和产物只写入被忽略的 `build/windows-vs2022/_deps`。系统依赖模式未启用。

Phase 1 历史 Linux 证据：

```text
runner: GitHub-hosted ubuntu-24.04
OS: Ubuntu 24.04.4 LTS
GCC: 13.3.0
CMake: 3.31.6
Debug configure/build: pass
Debug CTest: 43/43 pass, 0 failed
Release configure/build: pass
Release CTest: 43/43 pass, 0 failed
Release smoke: pass
project compiler warnings in CI log: 0
```

GitHub Actions：

```text
workflow: Linux CI (.github/workflows/linux-ci.yml)
run ID: 30508113122
run URL: https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122
event / attempt: push / 1
commit: 63b30cffcbe3e621af33664721b3675a647bd1a1
branch: phase/1-foundation
runner: ubuntu-24.04 (both jobs)
jobs: linux-debug, linux-release
run status / conclusion: completed / success
```

smoke 实际输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T02:19:47.674Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

Ubuntu/CMake 版本来自 Debug job 的 `Record environment`；Debug 与 Release configure 均识别 GNU 13.3.0，Release job 未单独重复打印 CMake 版本。两个 job 和所有必需步骤均为 `success`，workflow 最终 conclusion 为 `success`。

Phase 2 当前验证：

```text
Linux/WSL local run: unavailable
GitHub Actions run for current changes: none
Linux Debug configure/build/CTest: not run
Linux Release configure/build/CTest: not run
non-Linux IAISF_BUILD_LINUX_NETWORK=ON negative configure: expected failure
bash -n scripts: pass
workflow YAML parse: pass
forbidden implementation scan: pass
git diff --check: pass
sanitizers/static analyzers: not run/not installed
```

## 13. Linux 命令

```bash
./scripts/build_linux.sh Debug
./scripts/test_linux.sh Debug
./scripts/build_linux.sh Release
./scripts/test_linux.sh Release
./scripts/smoke_linux.sh
```

手工命令见 `docs/linux_build.md`。脚本从自身路径定位项目根目录，不执行 sudo、安装、Git 或源码删除。

## 14. 参考工程

只读目录：

```text
TinyWebServer_reference/
```

基线：

```text
files: 62
bytes: 59240225
aggregate sha256:
83AE7E469DEA30C860DEFD4D26CB313B7B3C87EFCD9387414741E152EE46CF27
```

任务结束复核结果完全一致：62 个文件、59,240,225 字节，聚合 SHA-256 仍为
`83AE7E469DEA30C860DEFD4D26CB313B7B3C87EFCD9387414741E152EE46CF27`。
参考目录被 `.gitignore` 排除。

## 15. 补充架构规则

以下是后续阶段约束，当前未创建对应类：

- TaskRepository 是 Succeeded/Failed/Timeout 竞争的唯一裁决者，首个终态获胜；
- Timeout 后晚到插件结果只记录并丢弃；
- 队列满未来映射 503，不误报插件错误；429 只用于用户级限流；
- 执行超时从 Running 开始，queue-wait timeout 是独立未来能力；
- signalfd 顺序：主线程屏蔽信号 → 创建 signalfd → 创建其他线程；
- ET 输出需要 buffer、高/低水位、最大 pending bytes、EPOLLOUT 启停、写到 EAGAIN 和慢客户端关闭；
- worker 只写完成队列并通过 eventfd 唤醒 EventLoop，不操作连接、Channel 或 epoll。

## 16. 未实现

- TcpConnection、Acceptor、TcpServer、bind/listen/accept/connect、连接缓冲和网络监听
- HTTP parser/request/response/session/router
- ThreadPool 和运行时队列
- TaskManager/Repository/Executor
- PluginManager/Registry/Echo/MockVision
- timerfd/signalfd 实现
- 异步日志、文件日志、轮转
- 数据库、登录、HTML、TLS
- benchmark、性能测试、真实 AI

禁止把 `worker_threads` 和 `task_queue_capacity` 配置字段解释为线程池已经实现。

## 17. 当前阻塞和遗留

1. Phase 1 无剩余验收阻塞；Phase 2 实现完成但缺少当前代码的真实 Linux CI 验证。
2. 默认 FetchContent 的首次 Linux 配置需要 GitHub 网络和 CA。
3. 系统依赖模式尚未在 Linux 系统包环境验证。
4. 本机没有可运行 Linux/WSL，不能在本机复现 CI。
5. Windows/NTFS 不可靠保存新 shell 脚本的 executable bit；workflow 已显式 `chmod +x scripts/*.sh`，人工 Linux 使用前仍需确认。
6. 本机 Visual Studio vcpkg applocal 集成的非致命 `pwsh.exe` 诊断与代码无关，但保留记录。

## 18. Phase 3 入口

进入条件：

1. 用户审阅并提交、push 当前 Phase 2 改动；
2. 对应 commit 在 Ubuntu 24.04 GCC Debug/Release 中 configure、build 和 CTest 全部通过；
3. 记录真实 run URL、commit SHA、测试数和 warning；
4. 用户明确允许开始 Phase 3。

Phase 3 预计从连接层和 HTTP 最小闭环开始，具体范围在开始前重新确认。当前禁止提前加入：

```text
ThreadPool
TaskRepository
PluginManager
timerfd/signalfd
async logging
AI plugins
```

## 19. Git 约束

- 当前分支必须保持 `phase/2-reactor-core`。
- 不 amend 或重写已有提交。
- 本轮不 commit、push、创建 PR 或 merge。
- build、FetchContent、compile_commands 和日志不能进入 Git。
- 最终建议 commit：

```text
feat: implement Linux reactor core
```
