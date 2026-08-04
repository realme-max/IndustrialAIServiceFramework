# 上下文交接

## 1. 当前状态

- 项目：IndustrialAIServiceFramework
- 当前分支：`phase/7-service-integration`
- Phase 0 提交：`5fbcec0 docs: complete phase 0 architecture design`
- Phase 1 最终实现提交：`63b30cffcbe3e621af33664721b3675a647bd1a1`
- Phase 2 起始 HEAD / main / origin/main：`6065d91b277c07ed04e64b3f08034788965e6ac1`
- Phase 2 Reactor 实现提交：`f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f`
- warning 修复 / 最终验证提交：`4db8708a5121f8477d835addd0b16170a3e2054f`
- Phase 2 文档封板 / Phase 3 基线：`e14b23131eb917df5758a10a305c2c87997f24cf`
- Phase 3 TCP 实现提交：`0a45658d0e450dd9dfde052808a27ae92ad08881`
- Phase 3 文档封板 / Phase 4 基线：`7096191ca8f7a3fe9e9acfb31ceba0a2c2fc3483`
- Phase 4 HTTP 实现提交：`9b87fdb8804ee37a8cf3b87a7b9193a3130b85d3`
- Phase 4 测试修复 / 最终验证提交：`0818ebf4f71366cc3cd2fe4e36e95fe667b687a5`
- Phase 4 文档封板 / Phase 5 基线 / main / origin/main：`fe5b58446a14ebedf13978b0339f3ad0171f0ffa`
- Phase 5 实现 / 最终验证提交：`79d3d4e89feb71595dc67d820f9a5398dcc814d4`
- Phase 5 文档封板 / Phase 6 基线 / main / origin/main：`b8e7ded21ce9b78d0d59e18785a4912356eb5e15`
- Phase 6 功能提交：`66a606bb53bf8ed80b8efd6faf7c6529b5cd22d1`
- Phase 6 warning 修复 / 最终验证提交：`853ccccca80cdc042b3d51eae52fe45566aa2b22`
- 当前阶段：Phase 7 Service Integration and Task HTTP API
- 状态：`PHASE_7_SERVICE_INTEGRATION_COMPLETED`
- 日期：2026-07-31
- Phase 1 GitHub Actions run：[30508113122](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122)
- Phase 2 首次功能 GitHub Actions run：[30514521602](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30514521602)
- Phase 2 最终零 warning GitHub Actions run：[30516007475](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475)
- Phase 3 最终 GitHub Actions run：[30524686201](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30524686201)
- Phase 4 首次失败 GitHub Actions run：[30537924856](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30537924856)
- Phase 4 最终 GitHub Actions run：[30539245789](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30539245789)
- Phase 5 最终 GitHub Actions run：[30547126540](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540)
- Phase 6 Linux 功能 GitHub Actions run：[30602538268](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30602538268)
- Phase 6 最终零 warning GitHub Actions run：[30604428624](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30604428624)
- Phase 2 实现、warning 修复和文档封板：已合并 main
- Phase 4 实现、修复与文档：已合并 main
- Phase 6 已合并 main；Phase 7 修复提交已存在并已由最新 Linux CI 验证
- 当前 Phase 7 HEAD：`1cc332b9d9e02ae78ec9e43455d36ffe939f73e2`；job checkout merge SHA：
  `466b80f742457da1fa67aea2859b770e361dcbe4`
- Phase 6 实现与 warning 修复已 commit/push；Phase 6F 只产生九份文档 diff

Phase 6C 审计后 Windows Debug/Release 均为 316/316：Foundation 43、HTTP Core 84、
Task Runtime 97、Plugin System 92。Release version/config smoke exit 0，项目
源码和测试编译 warning 为 0。Phase 6 首次 Linux push run `30602538268` 的
Debug/Release 均为 428/428，但两个配置各有 3 条项目源码 warning 与 3 条项目测试
warning，因此只作为功能通过记录。warning 修复后的最终 push run `30604428624`
同样完成 Debug/Release 428/428、Plugin System 92/92、Task Runtime 97/97 和 Release
smoke，且项目源码和测试 warning 均为 0，Phase 6 已完成封板。

## Phase 7 交接（当前工作区）

- 基线 main/origin/main：`9f88e0726a5db1abfe2ebe999de68aef41f317fb`
- 当前分支：`phase/7-service-integration`
- 当前状态：`PHASE_7_SERVICE_INTEGRATION_COMPLETED`
- 新目标：跨平台 `iaisf_task_api` / `iaisf::task_api`；Linux-only
  `iaisf_service` / `iaisf::service`
- Task API：`POST /v1/tasks`、`GET /v1/tasks/{id}`，加既有 `/health`、`/version`
- TaskId：统一 `TaskId::parse/to_string`；`task-` + 最少 16 位十进制，完整
  `uint64_t` 范围最长 20 位数字，字符串传输并严格 round-trip
- Router：exact 优先；只支持一个不解码、不规范化的末段参数；统一 route hard limit
- POST：严格 JSON，两字段 schema，提交前 Task/Plugin 校验，202 只表示 accepted
- GET：只序列化安全 Snapshot；不返回 input、时间戳、线程或异常原文
- 错误：400/404/405/413/415/422/500/503 稳定 JSON；405 保留 Allow
- admission：TaskHttpApi atomic gate；TaskManager 自身 in-flight submission 屏障仍是
  最终线性化边界
- typed submit：`submit_with_outcome()` 与原 submit 共用同一事务，只增加
  TaskSubmitFailure；HTTP 禁止按 message 分类
- Service 所有权：PluginManager → Adapter → TaskManager → TaskApi → HttpServer；
  销毁反向
- start：静态插件注册/freeze、adapter、task runtime、route freeze、HTTP start
- stop：`StoppingHttp -> StoppingTasks -> Stopped`；关闭 POST 后等待 HTTP stopped
  及 Session/connection 清空，再阻塞 drain/join tasks；不停止外部 EventLoop
- 202：只包含 task_id/status_url，不声称 queued
- Failed GET：HTTP 200 + 安全固定 error；不返回 result 或插件/异常原文
- 容量：ServiceOptions 在任何 worker/listener/Channel/route 前验证 pool、Task/Plugin
  JSON、request/snapshot envelope、HTTP header/body/target 与 TCP hard maximum
- 所有权：Router handler weak-own TaskHttpApi；TaskHttpApi 借用 Manager；Adapter
  closure 只持有 PluginManager；无 Service 回环
- Windows Debug/Release：370/370；Task API 46/46；Release smoke exit 0
- Linux 最新 push run 30781932731：Debug/Release 均实际执行完整 CTest 497/497；Service
  及 ActiveHttpStop 测试均通过，项目源码与测试 warning 均为 0
- CLI：仍不启动线程池、插件 registry 或常驻 listener
- 未实现：timerfd/signalfd、自动 timeout、cancel/retry/list、动态插件、真实 AI/GPU、
  DB、异步日志和 benchmark
- 下一步人工操作：Phase 8 仍未开始；遵守其范围边界后再规划下一阶段

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

## 4. Phase 3 文件

### 新建

```text
include/iaisf/net/tcp/buffer.hpp
include/iaisf/net/tcp/ipv4_endpoint.hpp
include/iaisf/net/tcp/acceptor.hpp
include/iaisf/net/tcp/tcp_connection.hpp
include/iaisf/net/tcp/tcp_server.hpp
src/net/tcp/buffer.cpp
src/net/tcp/ipv4_endpoint.cpp
src/net/tcp/acceptor.cpp
src/net/tcp/tcp_connection.cpp
src/net/tcp/tcp_server.cpp
tests/net/tcp/test_buffer.cpp
tests/net/tcp/test_ipv4_endpoint.cpp
tests/net/tcp/test_acceptor.cpp
tests/net/tcp/test_tcp_connection.cpp
tests/net/tcp/test_tcp_server.cpp
```

### 修改

```text
CMakeLists.txt
include/iaisf/net/channel.hpp
include/iaisf/net/event_loop.hpp
include/iaisf/net/socket.hpp
src/net/event_loop.cpp
src/net/socket.cpp
tests/CMakeLists.txt
tests/net/test_event_loop.cpp
.github/workflows/linux-ci.yml
README.md
docs/architecture.md
docs/development_plan.md
docs/test_plan.md
docs/stage_status.md
docs/context_handoff.md
docs/linux_build.md
```

没有创建 HTTP、ThreadPool、Task、Plugin、timerfd/signalfd、async logger、TLS、
Docker、benchmark 或 AI 代码。

## 4.1 Phase 4 文件

### 新建

```text
include/iaisf/http/http_status.hpp
include/iaisf/http/http_limits.hpp
include/iaisf/http/http_request.hpp
include/iaisf/http/http_response.hpp
include/iaisf/http/http_parser.hpp
include/iaisf/http/http_router.hpp
include/iaisf/http/builtin_routes.hpp
include/iaisf/http/http_session.hpp
include/iaisf/http/http_server.hpp
src/http/http_status.cpp
src/http/http_limits.cpp
src/http/http_request.cpp
src/http/http_response.cpp
src/http/http_parser.cpp
src/http/http_router.cpp
src/http/builtin_routes.cpp
src/http/http_session.cpp
src/http/http_server.cpp
tests/http/test_http_limits.cpp
tests/http/test_http_parser.cpp
tests/http/test_http_request_response.cpp
tests/http/test_http_router.cpp
tests/http/test_http_server.cpp
```

### 修改

```text
CMakeLists.txt
tests/CMakeLists.txt
.github/workflows/linux-ci.yml
include/iaisf/net/tcp/tcp_connection.hpp
src/net/tcp/tcp_connection.cpp
tests/net/tcp/test_tcp_connection.cpp
README.md
docs/architecture.md
docs/development_plan.md
docs/protocol.md
docs/test_plan.md
docs/stage_status.md
docs/context_handoff.md
docs/linux_build.md
```

为满足 HTTP close-after-response 契约，仅对 Phase 3 transport 增加了
`TcpConnection::close_after_write()` 和对应测试；没有改变 `shutdown()` 半关闭语义、
send 的 all-accepted-or-failure、ET I/O、Channel/Socket 所有权或 TcpServer 清理顺序。
未创建 ThreadPool、Task、Plugin、timerfd/signalfd、异步日志、AI、serve CLI、
Docker 或 benchmark。

## 4.2 Phase 5 文件

### 新建

```text
include/iaisf/task/thread_pool.hpp
include/iaisf/task/task_types.hpp
include/iaisf/task/task_limits.hpp
include/iaisf/task/task_repository.hpp
include/iaisf/task/task_executor.hpp
include/iaisf/task/task_manager.hpp
src/task/thread_pool.cpp
src/task/task_types.cpp
src/task/task_limits.cpp
src/task/task_repository.cpp
src/task/task_executor.cpp
src/task/task_manager.cpp
tests/task/test_thread_pool.cpp
tests/task/test_task_types.cpp
tests/task/test_task_limits.cpp
tests/task/test_task_repository.cpp
tests/task/test_task_executor.cpp
tests/task/test_task_manager.cpp
```

### 修改

```text
CMakeLists.txt
tests/CMakeLists.txt
.github/workflows/linux-ci.yml
include/iaisf/core/error.hpp
src/core/error.cpp
tests/test_error_result.cpp
README.md
docs/architecture.md
docs/development_plan.md
docs/protocol.md
docs/test_plan.md
docs/stage_status.md
docs/context_handoff.md
docs/linux_build.md
```

没有修改 Reactor/TCP/HTTP 生产代码、AppConfig schema、Application CLI 或 Linux
scripts；没有创建 HTTP task route、Plugin、timerfd/signalfd、异步日志或 AI 空壳。

## 4.3 Phase 6 文件

### 新建

```text
include/iaisf/core/json_value_limits.hpp
include/iaisf/plugin/algorithm_plugin.hpp
include/iaisf/plugin/echo_plugin.hpp
include/iaisf/plugin/mock_vision_plugin.hpp
include/iaisf/plugin/plugin_limits.hpp
include/iaisf/plugin/plugin_manager.hpp
include/iaisf/plugin/plugin_metadata.hpp
include/iaisf/plugin/plugin_task_adapter.hpp
src/plugin/echo_plugin.cpp
src/plugin/mock_vision_plugin.cpp
src/plugin/plugin_limits.cpp
src/plugin/plugin_manager.cpp
src/plugin/plugin_metadata.cpp
src/plugin/plugin_task_adapter.cpp
src/core/json_value_limits.cpp
tests/plugin/test_echo_plugin.cpp
tests/plugin/test_mock_vision_plugin.cpp
tests/plugin/test_plugin_limits.cpp
tests/plugin/test_plugin_manager.cpp
tests/plugin/test_plugin_metadata.cpp
tests/plugin/test_plugin_task_adapter.cpp
```

### 修改

```text
CMakeLists.txt
tests/CMakeLists.txt
.github/workflows/linux-ci.yml
include/iaisf/task/task_types.hpp
include/iaisf/task/task_limits.hpp
include/iaisf/task/task_manager.hpp
src/task/task_limits.cpp
src/task/task_manager.cpp
tests/task/test_task_manager.cpp
README.md
docs/architecture.md
docs/development_plan.md
docs/plugin_design.md
docs/protocol.md
docs/test_plan.md
docs/stage_status.md
docs/context_handoff.md
docs/linux_build.md
```

没有修改 Reactor/TCP/HTTP 生产代码、AppConfig、Application CLI 或 Linux scripts；
没有创建 HTTP Task API、动态加载、timerfd/signalfd、异步日志或真实 AI 代码。

## 5. CMake

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

iaisf_task (STATIC, portable)
  PUBLIC -> iaisf::core
  PUBLIC -> Threads::Threads
  PUBLIC -> nlohmann_json::nlohmann_json

iaisf_task_tests (portable)
  PRIVATE -> iaisf::task
  PRIVATE -> GTest::gtest / GTest::gtest_main / Threads::Threads

iaisf_plugin (STATIC, portable)
  PUBLIC -> iaisf::task
  PUBLIC -> iaisf::core
  PUBLIC -> nlohmann_json::nlohmann_json

iaisf_plugin_tests (portable)
  PRIVATE -> iaisf::plugin
  PRIVATE -> GTest::gtest / GTest::gtest_main / Threads::Threads

iaisf_net (STATIC, Linux only)
  PUBLIC -> iaisf::core
  PRIVATE -> Threads::Threads

iaisf_net_tests (Linux only)
  PRIVATE -> iaisf::net
  PRIVATE -> GTest::gtest
  PRIVATE -> GTest::gtest_main
  PRIVATE -> Threads::Threads

iaisf_tcp (STATIC, Linux only)
  PUBLIC -> iaisf::net

iaisf_tcp_tests (Linux only)
  PRIVATE -> iaisf::tcp
  PRIVATE -> GTest::gtest
  PRIVATE -> GTest::gtest_main
  PRIVATE -> Threads::Threads

iaisf_http_core (portable)
  PUBLIC -> iaisf::core

iaisf_http_core_tests (portable)
  PRIVATE -> iaisf::http_core
  PRIVATE -> GTest::gtest / GTest::gtest_main

iaisf_http (Linux only)
  PUBLIC -> iaisf::http_core
  PUBLIC -> iaisf::tcp

iaisf_http_tests (Linux only)
  PRIVATE -> iaisf::http
  PRIVATE -> GTest::gtest / GTest::gtest_main / Threads::Threads
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

## 6. 依赖

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

## 7. Error/Result 约定

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

## 8. AppConfig

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

## 9. Logger

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

## 10. Application

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

## 11. Phase 2 Reactor 契约

### 所有权与生命周期

- `UniqueFd` 独占 fd；`release()` 转移所有权，`reset()` 先关闭旧 fd；析构不抛异常，也不重试 `close(EINTR)`。
- `Socket` 拥有一个 `UniqueFd`，可以整体释放/接管；创建时使用 nonblocking 和 close-on-exec 标志，本阶段不执行 bind/listen/accept/connect。
- `Channel` 不拥有 fd，也不拥有 EventLoop；Channel 地址和 fd 必须在 epoll 注册期保持有效，析构前必须成功移除。Debug 析构断言用于暴露注册期销毁。
- `EpollPoller` 拥有 epoll fd，但不拥有注册的 Channel。
- `EventLoop` 拥有 Poller 和 eventfd wakeup Channel；注入的 `ILogger` 不归 EventLoop 所有，必须活得更久。
- active event vector 中的所有 Channel 必须活到整批分派结束。EventLoop 在分派期间拒绝直接 remove；应用回调使用 `queue_in_loop` 延迟，Acceptor/TcpServer 生命周期清理使用不受普通队列容量影响的 intrusive cleanup lane。

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
- `EpollPoller` 以 fd 为键保存 non-owning `Channel*` 注册表；add 拒绝重复 fd 并在 `epoll_ctl(ADD)` 失败时回滚表项，update/remove 要求 fd 与同一 Channel 地址匹配，remove 只在 `epoll_ctl(DEL)` 成功后清除表项和注册状态，poll 遇到未知 fd 返回 `InvalidState`。
- 重复 add、未注册 update/remove、非法 timeout 或错误 fd 返回稳定 `Result` 错误。
- eventfd 使用 nonblocking + close-on-exec；写侧将 `EAGAIN` 视为已有唤醒，读侧循环到 `EAGAIN`。
- 当前没有连接读写代码；未来 ET Socket handler 必须循环读/写直到 `EAGAIN`。

## 12. Phase 3 TCP 契约

### Endpoint、Buffer 与 Socket

- `Ipv4Endpoint` 只接受 numeric IPv4；不查询 DNS、不支持 IPv6。内部存 network-order
  address 和 host-order port，所有 `sockaddr_in` 都先清零。
- `Buffer` 不复制、可移动、owner-thread-only。maximum 必须 > 0，initial ≤ max；
  `retrieve` 只推进索引，append 仅在需要时 compact/grow。容量检查使用
  `length > max - readable`，超限和分配失败返回 `ResourceExhausted`。
- `Socket` 仍独占 `UniqueFd`；新增 bind/listen/local endpoint、`TCP_NODELAY`、
  `SO_ERROR`。`accept_ipv4` 使用 accept4 NONBLOCK+CLOEXEC，EAGAIN 是成功空值，
  EINTR/ECONNABORTED 重试。

### Acceptor 与所有权

- `Acceptor`、监听 Socket、Channel 的成员顺序保证 Channel 先销毁、Socket 后关闭；
  stop 先 remove Channel，再 reset Socket。active read callback 内 stop 时先进入
  Stopping，再由内嵌 cleanup 节点在完整 active batch 后执行该顺序。
- `TcpConnection` 中同样是 Socket 先声明、Channel 后声明；server table 的
  `shared_ptr` 是主要 owner，Channel callback 捕获 weak pointer。
- `TcpServer` shared-owned，但不拥有 EventLoop/ILogger；二者必须活得更久。对外
  延迟 callback 只捕获 weak server；内部清理 context 由内嵌节点承载，pending 节点
  析构会 `std::terminate`，不存在允许悬空的裸指针路径。
- close callback 把 connection strong pointer 加入按 `max_connections` 预分配的
  待移除向量，并幂等登记 intrusive cleanup 节点；普通 `queue_in_loop` 满不会拒绝
  清理。顺序是 `connect_destroyed/remove Channel/close fd` → erase table → 最后
  shared_ptr 释放。active batch 内不直接 remove 或析构。

### 状态、I/O 与容量

- Connection 状态：Connecting → Connected → Disconnecting → Disconnected；
  close notification 有门闩，最多一次。connection callback 在 established 和
  destroyed 后各调用一次；从未 established 的失败连接不发虚假 disconnect 通知。
- ET read/accept/write 都 drain 到 EAGAIN；EINTR 重试。message callback 在 owner
  线程收到 `Buffer&` 并负责 retrieve；TCP 没有消息边界。
- send 只允许 owner 线程并采用 all-accepted-or-failure：先使用减法式上限检查和
  `ensure_writable` 为整个 payload 预留，再登记 `EPOLLOUT`，最后一次性 append。
  failure 时本次 payload 没有任何前缀被发送或缓存；success 后可写回调用
  `MSG_NOSIGNAL` drain，部分写/EAGAIN 的完整后缀已在 Buffer。
- input/output hard maximum 是 fail-closed；high-water 只做 below→at/above 通知，
  below 后重武装，不自动暂停读，不等同于流量治理。
- peer EOF 后停止读取但交付本轮新增数据一次；部分或完全未消费数据不会再次触发
  message callback，并在销毁时丢弃。有输出先排空。主动 graceful shutdown 先排空
  再 shutdown write 并等待 peer；无 timerfd，可能长期 Disconnecting。
- `TcpServer::stop` owner-thread-only、幂等、停止 accept、force-close 现有连接且
  永久禁止 restart；正常 stop 不停止 EventLoop。非 active 调用同步清空连接表并
  返回 Stopped；active callback 内调用先返回 Stopping，批次后清理完成时
  `stopped()` 才为 true。stop 期间只保留一次 Disconnected 通知，不再处理消息。
- started server 必须 stop-complete 后析构；否则运行时终止。Created 且从未启动的
  空 server 可由析构安全关闭。
- 单调 connection id 在到达 `uint64_t` wrap 前拒绝后续连接，不发生回绕。

### 回调异常矩阵

| 回调 | 策略 | 是否影响清理 |
|---|---|---|
| connection/Connected | 记录并关闭该连接 | 否 |
| connection/Disconnected | 记录；Channel/fd 已移除 | 否 |
| message | 记录并关闭该连接 | 否 |
| high-water | 记录，连接继续 | 否 |
| Acceptor new-connection | accepted Socket RAII 关闭，accept loop 继续 | 否 |
| close | server 安装 `noexcept` weak 内部 hook；低层误用异常请求 loop stop | server 路径不会抛 |
| write-complete | 未实现 | 不适用 |

所有 Logger 调用都有不抛异常的保护；内部连接清理不依赖任何用户 callback 成功返回。

### 线程边界

线程安全入口只有：

```text
EventLoop::queue_in_loop
EventLoop::stop
```

Acceptor/TcpServer start/stop、TcpConnection send/shutdown/close_after_write/force_close、
callback setter、Buffer 操作、Channel update/remove 和连接表操作都只允许
EventLoop owner 线程。未来 worker 不得直接操作这些对象。

`TcpServerOptions` 目前由代码 factory/defaults 构造，尚未接入 `AppConfig`。
accepted socket 当前固定启用 TCP_NODELAY；可选的受验证 `socket_send_buffer_bytes`
默认不覆盖系统值，测试用它确定性制造 backpressure。`iaisf_server` 仍不创建
EventLoop 或 TcpServer。

## 12.1 Phase 4 HTTP 契约

### Core 与 framing

- `HttpLimits` 包含 method/target/request-line/header-line/header-total/header-count/
  request-body/response-body/routes/requests-per-dispatch 十项不可变硬限制。有符号
  factory 拒绝负/零/超硬上限和跨字段矛盾，不 clamp。
- 只支持 HTTP/1.1、origin-form、strict CRLF 和 Content-Length。method 保留大小写；
  target/path/raw query 不 decode/normalize；header name lowercase、value trim OWS，
  body binary-safe。
- request line/header line 限制包含 CRLF，header total 包含每行 CRLF 和终止空行；
  所有重复的规范化 header name 返回 400。Host 必须恰好一个；CL+TE、非法 CL
  返回 400；CL 数字溢出或 body 超限为 413；target/request-line 为 414；header
  限制为 431；TE/Upgrade 为 501；Expect 为 417；非 1.1 为 505。组合错误检查
  顺序固定，不依赖无序容器迭代。
- `Connection` 使用 comma-separated、大小写不敏感的完整 token；相似子串不匹配，
  空 token 或非法 token 拒绝。
- Parser 状态是 RequestLine→Headers→Body→Complete 或 terminal Error；parse 返回
  consumed count，take 后 reset。bad_alloc/length_error 转 Result，Session 映射 500。
- Response 自动生成 Content-Length 和 Connection，拒绝 handler 设置 framing header
  或 CRLF 注入；序列化 preflight 检查 body、header count/line/head total，包含
  自动 framing、状态行、CRLF 与终止空行；错误响应固定 plain text + close，失败
  不产生部分字符串。
- 请求与响应共享 Header 容量；若配置小到无法容纳框架错误响应，preflight 失败后
  Session 直接 fail-closed，不发送截断或缺少 Content-Length/Connection 的降级响应。
  固定错误 Content-Type 行含 CRLF 为 41 字节，portable exact-limit 测试覆盖该边界。
- Router exact method+path、query 不参与、freeze 后只读；404、405/sorted Allow；
  handler Error、标准/未知异常变成 closed 500，内部文本不外泄。
- Built-ins 只包含 GET `/health` 和 `/version`。health 只说明 HTTP/EventLoop 可响应，
  不表示 Task/Plugin/GPU/数据库 healthy。

### Session/Server 所有权和线程

- HttpServer owns TcpServer、frozen Router、connection-id→Session table；不 owns
  EventLoop/ILogger。EventLoop/Logger 必须活得更久。
- TcpServer 保持 TcpConnection primary shared ownership；Session 只持 connection
  weak pointer，不注册/拥有 Channel，不改变 Socket→Channel→table 销毁顺序。
- server callbacks 捕获 weak server；Session continuation 捕获 weak session +
  weak connection，不捕获悬空 `this`，同时至多一个；执行前要求 Session 非 terminal
  且连接仍为 Connected。
- 所有 HTTP dispatch/send/start/stop owner-thread-only。每轮最多
  `max_requests_per_dispatch`，剩余 pipeline 通过普通有界 queue 继续；入队失败关闭。
- `Connection: close`、parser/router/serialize/send/internal error 后不处理后续
  pipeline；malformed 至多一个错误响应。HTTP close 使用 `close_after_write()` 写尽
  后主动全关闭，不等待 peer EOF；`shutdown()` 保持独立半关闭语义。
- close callback 后的连接表/Session 表移除沿用 TcpServer 内部 `DeferredCleanup`，
  在 active Channel 批次后执行且不受普通 pending queue 容量影响。
- HttpServer stop 镜像 TcpServer，幂等、禁止 restart；active batch 内可能延迟完成，
  `stopped()` 同时要求 TCP stopped 和 Session 表为空；started server 未完成 stop
  不能析构。
- CLI 保持 Phase 1 行为，不创建 EventLoop/HttpServer，也没有 `--serve`。

## 13. 测试

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

Phase 2 封板提交有 44 个 Linux-only `TEST` 定义并已实际验证；Phase 3B 为内部
cleanup lane 新增 1 项，当前工作区 Reactor 定义为 45：

```text
UniqueFd: 8
Socket: 5
Channel: 7
EpollPoller: 7
EventLoop: 18
```

Phase 2B 增加/加强了 RDHUP/HUP 分派、fd 复用、状态矩阵、Stopping 拒绝、容量竞争、失败回调不执行、唤醒失败回滚、active 批次延迟移除、回调异常边界、eventfd drain 和饱和计数器 EAGAIN。Phase 3B 新增的第 45 项验证普通 callback queue 满时 intrusive cleanup 仍只执行一次。测试使用 eventfd/socketpair，不监听端口；并发等待均有 condition variable/future 的有限超时，CTest timeout 为 10 秒。最终 Phase 3 Linux Debug/Release 均实际执行 Reactor 45/45。

Phase 3 当前源码 `TEST` 定义：

```text
Buffer: 13
Ipv4Endpoint: 4
Acceptor/Socket server operations: 7
TcpConnection: 7
TcpServer integration: 20
Total definitions: 51
```

最终 Linux Debug/Release 均实际发现并执行 TCP 50/50；与 Foundation 43、Reactor 45
合计 138/138。测试只使用 loopback/port 0 或
socketpair，有限 future/condition variable/socket timeout，无 fixed sleep、无
detached thread；CTest timeout 20 秒。覆盖 binary/fragment/large Echo、burst、
multi-client、容量、dynamic EPOLLOUT/high-water rearm、half-close、RST、异常隔离、
close once、普通队列满清理、active stop、多连接清理、EOF 部分/不消费、析构契约、
active batch 后延迟 remove、最后强引用释放和 stop cleanup。

Phase 3 最终 Linux 历史结果仍为 TCP 50/50；上面的第 51 项是 Phase 4 新增的
`close_after_write()` 传输契约测试，已由 Phase 4 最终 Linux run 验证，但不回写
Phase 3 历史矩阵。

Phase 4 HTTP 测试定义：

```text
HttpLimits: 8
HttpParser: 40
HttpRequest/Response/Status: 21
HttpRouter/Builtins: 15
Portable HTTP Core total: 84
Linux HttpSession/HttpServer integration: 16
```

Windows Debug/Release 均实际为 Foundation 43/43 + HTTP Core 84/84 = 127/127；
Release version/config smoke exit 0，项目 warning 0。既知 vcpkg applocal
`pwsh.exe` 诊断非项目 warning。最终 Linux Debug/Release 均实际执行 239/239，
HTTP Integration 16/16。

## 14. 环境和实际结果

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

Phase 3 当前工作区 Windows 回归（网络 target 关闭，2026-07-30）：

```text
configure: pass
Debug clean build: pass
Debug CTest: 43/43 pass
Release clean build: pass
Release CTest: 43/43 pass
Release --version: exit 0, IndustrialAIServiceFramework 0.1.0
Release --config example: exit 0, configuration validated
project C++ warnings: 0
non-Linux network ON negative configure: expected failure
bash -n: pass
workflow YAML parse: pass
```

最终项目自身 MSVC warning 为 0。Visual Studio 的本机 vcpkg applocal 集成会尝试调用缺失的 `pwsh.exe` 并打印非致命诊断，Debug/Release build 仍为 exit 0、CTest 均 43/43、smoke 均 exit 0；项目本身没有启用系统依赖模式。Windows 结果只证明 Phase 1 可移植基线未回归，不能编译或验证 Linux Reactor/TCP。

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

Phase 2 首次功能 CI：

```text
run ID: 30514521602
run URL: https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30514521602
commit: f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f
result: Debug/Release configure/build/CTest/smoke pass
CTest: Debug 87/87; Release 87/87; Reactor 44/44 each
warnings: Debug 0; Release 2 x -Wunused-result in Reactor test source
role: first functional pass, not final zero-warning evidence
```

Phase 2 最终验证：

```text
Linux/WSL local run: unavailable
workflow: Linux CI
run ID / attempt: 30516007475 / 1
run URL: https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475
event / branch: push / phase/2-reactor-core
head SHA: 4db8708a5121f8477d835addd0b16170a3e2054f
checkout SHA: 4db8708a5121f8477d835addd0b16170a3e2054f (both jobs)
runner / OS: ubuntu-24.04 / Ubuntu 24.04.4 LTS
kernel: 6.17.0-1020-azure x86_64
GCC / CMake: 13.3.0 / 3.31.6
Linux Debug configure/build: pass
Linux Debug CTest: 87/87 pass, 0 failed; Reactor 44/44
Linux Release configure/build: pass
Linux Release CTest: 87/87 pass, 0 failed; Reactor 44/44
Release smoke: pass
run status / conclusion: completed / success
non-success jobs/steps: 0
continue-on-error: 0
project source warnings: Debug 0; Release 0
project test warnings: Debug 0; Release 0
targets: iaisf_net and iaisf_net_tests built in both jobs
non-Linux IAISF_BUILD_LINUX_NETWORK=ON negative configure: expected failure
bash -n scripts: pass
workflow YAML parse: pass
forbidden implementation scan: pass
git diff --check: pass
sanitizers/static analyzers: not run/not installed
```

Phase 2 Release smoke 实际输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T05:14:04.588Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

两个 job 均实际构建 `iaisf_net` 和 `iaisf_net_tests`，并各执行 44/44 Reactor
测试。最终日志中的两处 `-Wunused-result` 已消失；checkout 阶段 `git init` 默认
分支提示包含单词 “warning”，但不是项目源码、项目测试或第三方依赖编译 warning。
所有 job/step 均成功，没有 failed、cancelled、skipped、neutral、失败重试或
`continue-on-error` 掩盖。

Phase 3 Linux 最终验证：

```text
workflow: Linux CI
run ID / attempt: 30524686201 / 1
run URL: https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30524686201
event / branch: push / phase/3-tcp-transport
head SHA: 0a45658d0e450dd9dfde052808a27ae92ad08881
checkout SHA: 0a45658d0e450dd9dfde052808a27ae92ad08881 (both jobs)
local HEAD / upstream（Phase 3 验证当时）: 0a45658d0e450dd9dfde052808a27ae92ad08881
runner / OS: ubuntu-24.04 / Ubuntu 24.04.4 LTS
kernel: 6.17.0-1020-azure x86_64
GCC / CMake: 13.3.0 / 3.31.6
Linux Debug configure/build: pass
Linux Debug CTest: 138/138 pass, 0 failed
Linux Release configure/build: pass
Linux Release CTest: 138/138 pass, 0 failed
actual suites per configuration: Foundation 43/43; Reactor 45/45; TCP 50/50
targets: iaisf_tcp and iaisf_tcp_tests built in both jobs
Release smoke: version/config pass
project source warnings: Debug 0; Release 0
project test warnings: Debug 0; Release 0
run status / conclusion: completed / success
non-success jobs/steps: 0
continue-on-error: 0
status: PHASE_3_TCP_TRANSPORT_COMPLETED
```

Release smoke 实际输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T07:58:10.003Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

两个 job 均实际构建 `iaisf_tcp` 和 `iaisf_tcp_tests`。Actions job/step conclusion
全部为 success；CMake 的 compiler capability “skipped” 行不是被跳过的 Actions
步骤。完整 build/test 日志没有项目源码或测试 warning；checkout 的 `git init`
默认分支提示不是项目 warning。

Phase 4 首次失败与最终验证：

```text
implementation commit: 9b87fdb8804ee37a8cf3b87a7b9193a3130b85d3
first failing run ID: 30537924856
first failing result: Debug 237/238; Release 237/238
failure: HttpServerTest.RejectsFramingAmbiguitiesAndConfiguredLimits
root cause: response header-line fixture 32 bytes < fixed error Content-Type line 41 bytes
behavior: parser mapped missing Host to 400; error response preflight failed; session fail-closed
fix commit: 0818ebf4f71366cc3cd2fe4e36e95fe667b687a5

workflow: Linux CI
final run ID / attempt: 30539245789 / 1
final run URL: https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30539245789
event / branch: push / phase/4-http-protocol
head SHA: 0818ebf4f71366cc3cd2fe4e36e95fe667b687a5
checkout SHA: 0818ebf4f71366cc3cd2fe4e36e95fe667b687a5 (both jobs)
local HEAD / upstream: 0818ebf4f71366cc3cd2fe4e36e95fe667b687a5
runner / OS: ubuntu-24.04 / Ubuntu 24.04.4 LTS
kernel: 6.17.0-1020-azure x86_64
GCC / CMake: 13.3.0 / 3.31.6
Linux Debug configure/build: pass
Linux Debug CTest: 239/239 pass, 0 failed
Linux Release configure/build: pass
Linux Release CTest: 239/239 pass, 0 failed
actual suites: Foundation 43; Reactor 45; TCP 51; HTTP Core 84; HTTP Integration 16
original failing test: pass in Debug and Release
targets: iaisf_http_core, iaisf_http_core_tests, iaisf_http, iaisf_http_tests built
project source warnings: Debug 0; Release 0
project test warnings: Debug 0; Release 0
run status / conclusion: completed / success
non-success jobs/steps: 0
continue-on-error: 0
```

Phase 4 Release smoke 实际输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T11:38:17.579Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

最终采用 push run；同 SHA 的成功 PR run checkout 为 GitHub 临时 merge commit，
不用于满足 checkout/local/upstream SHA 完全一致的封板条件。最终 push run 两个 job
和全部步骤均为 success，没有 failed、cancelled、skipped、neutral、timeout 或
`continue-on-error`，编译日志中项目源码和测试 warning 为 0。

## 14.1 Phase 5 Windows 验证

实际命令：

```powershell
cmake -S . -B build/windows-vs2022 -G "Visual Studio 17 2022" -A x64 -DIAISF_BUILD_TESTS=ON -DIAISF_USE_SYSTEM_DEPS=OFF -DIAISF_BUILD_LINUX_NETWORK=OFF
cmake --build build/windows-vs2022 --config Debug --clean-first --parallel
ctest --test-dir build/windows-vs2022 -C Debug --output-on-failure
cmake --build build/windows-vs2022 --config Release --clean-first --parallel
ctest --test-dir build/windows-vs2022 -C Release --output-on-failure
```

实际矩阵：

```text
Debug:   Foundation 43 + HTTP Core 84 + Task Runtime 85 = 212/212
Release: Foundation 43 + HTTP Core 84 + Task Runtime 85 = 212/212
failed: 0
project compiler warnings: 0
```

Task Runtime 分项：

```text
BoundedThreadPool 18
TaskId / TaskState 3
TaskLimits 8
TaskRepository 20
TaskExecutor 14
TaskManager 22
total 85
```

Release smoke：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T13:23:45.738Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

`bash -n` 无法在当前宿主执行，因为命令不存在；没有声称本机 Linux 构建通过。

## 14.2 Phase 5 Linux 最终验证

```text
workflow: Linux CI
run: 30547126540
URL: https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540
attempt / event / conclusion: 1 / push / success
branch: phase/5-task-runtime
head / Debug checkout / Release checkout:
79d3d4e89feb71595dc67d820f9a5398dcc814d4
runner: ubuntu-24.04
OS: Ubuntu 24.04.4 LTS
kernel: 6.17.0-1020-azure
compiler: GCC 13.3.0
CMake: 3.31.6
Debug: configure/build success; 324/324 CTest; 0 failed
Release: configure/build success; 324/324 CTest; 0 failed
modules: Foundation 43; Reactor 45; TCP 51; HTTP Core 84;
         HTTP Integration 16; Task Runtime 85
task targets: iaisf_task and iaisf_task_tests built in Debug and Release
project source warnings: 0
project test warnings: 0
```

Release smoke：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T13:31:28.474Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

两个 job 和全部可见步骤均为 success；没有 failed、cancelled、skipped、neutral、
timeout 或 `continue-on-error`。run head、两个 checkout、本地 HEAD 与 upstream
完全一致。

## 15. Linux 命令

```bash
./scripts/build_linux.sh Debug
./scripts/test_linux.sh Debug
./scripts/build_linux.sh Release
./scripts/test_linux.sh Release
./scripts/smoke_linux.sh
```

手工命令见 `docs/linux_build.md`。脚本从自身路径定位项目根目录，不执行 sudo、安装、Git 或源码删除。

## 16. 参考工程

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

Phase 4 开始、实现交付和 Phase 4C 封板前复核结果完全一致；Phase 5 开始、交付前
和 Phase 5C 封板前再次复核为 62 个文件、59,240,225 字节，聚合 SHA-256 仍为
`83AE7E469DEA30C860DEFD4D26CB313B7B3C87EFCD9387414741E152EE46CF27`。
Phase 6 开始与交付前复核也完全一致；参考目录被 `.gitignore` 排除。

## 17. Phase 5 Task Runtime 契约

- `ThreadPoolOptions` 使用 `worker_threads`/`queue_capacity`，都要求正数并具有硬上限。
- `BoundedThreadPool::try_submit` 不阻塞等待空位；FIFO 队列满返回 ResourceExhausted。
- worker 不持队列 mutex 执行闭包，捕获标准与未知异常；无 detached thread。
- Pool 状态为 Running→ShuttingDown→Stopped；try_submit/shutdown 共用 mutex
  线性化。并发 shutdown 只有一个 joiner，其余等待 Stopped；join 后清空 thread
  对象。worker self-shutdown 在状态变化前返回 InvalidState，外部仍可正常停止。
- `TaskId` 在线程安全 Repository 内单调生成；rollback/erase 后不复用，分配到
  `uint64_t` 最大值后永久返回 ResourceExhausted，不回绕。
- TaskSnapshot 是独立副本；时间满足 created ≤ started ≤ finished。
- TaskLimits 校验 JSON depth/elements/string 与紧凑 UTF-8 serialized bytes；计数
  stream 不保留整份 dump，discarded/non-finite/非法 UTF-8 fail-closed；error
  超限使用 `#` 泛化。
- TaskRepository 是 Succeeded/Failed/TimedOut 竞争的唯一裁决者，首个终态获胜。
- TimedOut 后晚到 success/failure 返回 AlreadyTerminal，不覆盖 result/error；
  TimedOut 被 erase 后晚到完成返回 NotFound，同样计数并丢弃。
- `TaskHandler` 是唯一执行注入边界，可能由多个 worker 并发调用，必须线程安全。
- TaskExecutor 不持仓库锁调用 handler，也不接触网络；异常原文不进入 Snapshot。
  result 超限或 JSON 序列化失败使用固定内部错误进入 Failed；handler Error 经过
  max_error_message_bytes 归一化。
- TaskManager submit 的 admission 线性化点是在 mutex 下检查 accepting 并增加
  in-flight 计数；随后锁外 create/try_submit/rollback，RAII guard 保证计数归还。
- TaskManager shutdown 的线性化点是把 accepting 设为 false；先通过 condition
  variable 等待 in-flight submission 归零，再 drain/join。并发 caller 幂等。
- 成员声明顺序为 admission state、Repository、Executor、Pool；析构体先 shutdown，
  逆序销毁时 Pool 先于 Executor/Repository。closure 捕获稳定 Executor 地址，不捕获
  TaskManager this；Logger 和 handler 捕获的外部依赖必须比 Manager 活得更久。
- Repository 无自动 eviction、TTL、持久化或重启保留；显式 erase terminal 释放容量。
- 同一个 handler 可被多个 worker 并发调用且不会被 Executor 串行化；调用者必须
  保证线程安全，不得依赖 EventLoop owner thread 或操作网络对象。
- 非协作 handler 可能无限延迟 shutdown；Phase 5 不强杀线程。

继续保留的网络边界：

- signalfd 顺序：主线程屏蔽信号 → 创建 signalfd → 创建其他线程；
- Phase 3 已实现 output hard maximum、high-water crossing、EPOLLOUT 启停和写到
  EAGAIN；自动 pause-read、低水位回调和 write deadline 尚未实现；
- Phase 5 worker 不捕获或操作 TcpConnection、HttpSession、Channel、Socket、
  EventLoop 或 epoll；HTTP/Task completion adapter 尚未实现。

## 17.1 Phase 6 Plugin System 契约

- `PluginLimits` 使用有符号 factory，所有值 > 0、有硬上限、不 clamp；覆盖 registry、
  metadata/error、input/output serialized bytes、JSON depth/elements/string 以及
  capability count/name bytes。
- `PluginMetadata` 是独立副本；operation/capability 只接受 lowercase ASCII
  `a-z0-9._-`，不允许首尾点/连续点；capabilities 唯一，文本拒绝控制字符和非法 UTF-8。
- `IAlgorithmPlugin` 只有 metadata/validate/execute；validate 必须快速、确定、重复、
  无 I/O/外部副作用；同一实例 validate/execute 可同时运行，插件作者负责线程安全，
  接口无网络/HTTP/Repository/文件/GPU 类型。
- PluginManager 强持有 `shared_ptr<const IAlgorithmPlugin>`，禁止 copy/move；
  注册只调用一次 metadata 并保存副本，operation 唯一。
- Registry 为 Configuring→Frozen；register/freeze 在同一 mutex 线性化，freeze 幂等
  不可逆。Configuring 只允许 register/freeze，find/list/validate/execute 均返回
  InvalidState 且不调用插件；Frozen 后注册在 metadata 前快速拒绝，并发
  lookup/list/validate/execute，调用插件前复制 handle 并释放 mutex。
- 注册的 null/duplicate/capacity/invalid metadata/exception 都不改变 size；map
  insertion 失败不留下半项。注册键必须在移动 metadata 前复制，避免参数求值顺序缺陷。
- Unknown operation 使用 ErrorCode::NotFound；不通过 message 判断。
- validation exception 固定为 `plugin validation failed`；execution exception 和
  execute 返回的任意插件 Error 固定为 `plugin execution failed`，不泄露
  `what()`/路径；全部对外 message 受 PluginLimits 限制。
- `validate_json_value` 先有界遍历 JSON 结构，再用 counting stream 精确计算
  nlohmann compact dump 的 UTF-8 输出 bytes，不生成 `dump()` 整文档副本；引号、
  反斜杠、控制字符、UTF-8、object key 的转义均计入，首次超过上限立即中止。输入
  discarded/non-JSON binary/非法 UTF-8/non-finite 为 InvalidArgument；容量为
  ResourceExhausted；插件非法输出为 InternalError。
- 公共 `PluginManager::execute` 不能绕过输入/plugin validation，成功输出在 Manager
  返回和 Repository 写入前统一检查；失败不保存半结果。
- TaskManager 旧 handler-only create 保留；新重载增加 TaskValidator。submit 顺序是
  admission/in-flight → TaskLimits → validator → create_queued → try_submit/rollback。
- validator 在 admission/Repository mutex 外运行，failure 不分配 ID/record/queue；
  shutdown 等待 in-flight validator 完成，validator 可由多个 submitter 并发调用。
- PluginTaskAdapter 只接受 Frozen Manager；validator 提交前校验，handler 对
  worker-owned TaskRequest 再次校验。结果变化固定为 InternalError 且不调用 execute。
  Adapter 强持有只读 Manager；生成的 validator/handler closure 也只强持有该
  Manager，不持有 Adapter、TaskManager 或裸 `this`。TaskManager 保存 validator，
  TaskExecutor 保存 handler，worker queue closure 只持非 owning Executor 指针、
  TaskId 和请求副本；所有 worker join 并释放两个 closure 后 Manager/Plugin 可析构，
  所有权图无回边、无 shared ownership cycle。
- 成功 submit 前 Repository/closure 已拥有 operation/input snapshot；调用方修改或
  销毁原 request、源字符串和 JSON 不影响已接收任务。
- Echo operation=`echo`、mock=false，严格接收 `{"payload": value}` 并把 payload
  的任意 JSON kind 原值复制为结果，不增加 `echo`/`operation` wrapper；MockVision
  operation=`mock_vision.detect`、mock=true，严格 image_id/width/height/threshold。
- MockVision 不接受路径字段、不读取图片/点云、不运行 OpenCV/PCL/TensorRT/GPU，
  固定 confidence 0.93 和确定性 bbox 只用于框架验证，不代表准确率/性能。

Windows Phase 6 实际结果：

```text
Debug:   Foundation 43 + HTTP Core 84 + Task Runtime 97 + Plugin System 92 = 316/316
Release: Foundation 43 + HTTP Core 84 + Task Runtime 97 + Plugin System 92 = 316/316
Release version/config smoke: exit 0
project source/test compiler warnings: 0
```

Linux 首次功能 CI：

```text
workflow/event/attempt: Linux CI / push / 1
run: https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30602538268
run id: 30602538268
head/debug checkout/release checkout: 66a606bb53bf8ed80b8efd6faf7c6529b5cd22d1
runner: ubuntu-24.04
OS: Ubuntu 24.04.4 LTS
kernel: 6.17.0-1020-azure
GCC: 13.3.0
CMake: 3.31.6
Debug: configure/build success; 428/428 CTest
Release: configure/build success; 428/428 CTest
modules: Foundation 43, Reactor 45, TCP 51, HTTP Core 84,
         HTTP Integration 16, Task Runtime 97, Plugin System 92
Release smoke: version/config success
project source warnings: 3 per configuration
project test warnings: 3 per configuration
```

两个 job 和所有步骤均 success，无 failed/cancelled/skipped/neutral/timeout 或
`continue-on-error`。并发、freeze/register 竞态、输入快照、JSON 转义字节限制、
Echo 任意 JSON 原值和 MockVision `mock: true` 测试均实际执行。由于 warning 不为
0，因此不是最终封板证据。

Linux 最终零 warning CI：

```text
workflow/event/attempt: Linux CI / push / 1
run: https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30604428624
run id: 30604428624
head/debug checkout/release checkout: 853ccccca80cdc042b3d51eae52fe45566aa2b22
runner: ubuntu-24.04
OS: Ubuntu 24.04.4 LTS
kernel: 6.17.0-1020-azure
GCC: 13.3.0
CMake: 3.31.6
Debug: configure/build success; 428/428 CTest
Release: configure/build success; 428/428 CTest
modules: Foundation 43, Reactor 45, TCP 51, HTTP Core 84,
         HTTP Integration 16, Task Runtime 97, Plugin System 92
targets: iaisf_plugin and iaisf_plugin_tests built in both jobs
Release smoke: version/config success
project source warnings: 0
project test warnings: 0
```

workflow、两个 job 和全部步骤均为 success，没有 failed、cancelled、skipped、
neutral、timeout、`continue-on-error` 或被隐藏的失败步骤。

## 18. 未实现

- HTTP Task API、`/v1/tasks` 或 `/api/v1/tasks` 路由
- 动态 `.so`/DLL、插件发现、热加载/卸载、配置生命周期和进程隔离
- timerfd/signalfd、自动 timeout、取消/重试/优先级
- 异步日志、文件日志、轮转
- 数据库、登录、HTML、TLS
- benchmark、性能测试、真实 AI
- sanitizer、benchmark 和 `/proc` fd 长时间稳定性统计

Task Runtime 与可组合 Task HTTP API 已实现，但禁止把现有 AppConfig 字段解释为
CLI 已启动线程池或 listener；网络访问只存在于显式构造 Linux Service 的调用方。

## 19. 当前阻塞和遗留

1. Phase 1—6 无剩余验收阻塞；Phase 7 等待 exact commit 的 Linux CI。
2. 默认 FetchContent 的首次 Linux 配置需要 GitHub 网络和 CA。
3. 系统依赖模式尚未在 Linux 系统包环境验证。
4. 本机没有可用 WSL/Linux，不能执行本地 Linux build/CTest；只做 shell 静态检查。
5. Windows/NTFS 不可靠保存 shell executable bit；workflow 会执行 `chmod +x scripts/*.sh`。
6. 本机 Visual Studio vcpkg applocal 集成的非致命 `pwsh.exe` 诊断与代码无关。
7. graceful shutdown 没有 timerfd deadline，非协作 peer 可能长期 Disconnecting；
   server stop 因此使用 force-close。
8. 非协作 TaskHandler 会阻塞 worker 并延迟 TaskManager shutdown；没有安全强杀。
9. Repository 终态不会自动清理；调用者必须显式 `erase_terminal`。

## 20. Phase 8 入口

Phase 7 只有在真实 Linux CI 对当前提交完成 Debug/Release 全量 CTest、Service
loopback tests、Release smoke 和零项目 warning 后才能封板。之后 Phase 8 可规划：

```text
timerfd connection idle timeout
automatic task timeout arbitration
signalfd-driven process shutdown integration
bounded asynchronous logger and validated service configuration
```

Phase 8 仍暂不包含：

```text
dynamic .so
real TensorRT/PCL/point-cloud/GPU/robot
database
benchmark
```

## 21. Git 约束

- 当前分支必须保持 `phase/7-service-integration`。
- 不 amend 或重写已有提交。
- 本轮不 commit、push、创建 PR 或 merge。
- build、FetchContent、compile_commands 和日志不能进入 Git。
- 本轮不 commit/push；建议 commit：

```text
feat: integrate task HTTP service
```

## Phase 7E 最终 Linux CI 审计记录（2026-08-03）

- 当前分支：`phase/7-service-integration`；当前 HEAD/Phase 7 修复提交：`1cc332b9d9e02ae78ec9e43455d36ffe939f73e2`（`fix: stabilize active HTTP stop lifecycle test`）。
- Linux CI：[run 30779555703](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30779555703)，workflow `Linux CI`，`pull_request`，attempt 1，conclusion `success`；job checkout merge SHA `466b80f742457da1fa67aea2859b770e361dcbe4`。
- 环境：runner `ubuntu-24.04`，Ubuntu 24.04.4 LTS，kernel `6.17.0-1020-azure`，GCC 13.3.0，CMake 3.31.6。
- Debug/Release：configure、build 均成功，CTest 均为 `497/497`，0 failed；Release version/config smoke 成功。
- Active HTTP stop：`IndustrialAiServiceTest.ActiveHttpStopWaitsForDeferredCleanupBeforeJoiningTasks` 在 Debug/Release 均通过。
- 历史状态：项目测试 warning 曾非零：`tests/service/test_industrial_ai_service.cpp:1041:51` 的 `-Wshadow`，每 job 3 次、共 6 条记录；后续 warning 修复提交和最终 run 已清零。尚未执行 `ctest --repeat until-fail:50`。

### 生命周期不可破坏约定

active HTTP 请求触发 stop 时，当前请求不保证返回 503；TcpServer/HttpServer cleanup 可能先关闭连接，已开始发送的响应不得被截断。普通 Stopping 阶段新 POST 仍映射 503。销毁顺序必须是：`TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`；DeferredCleanup 先于阻塞任务 shutdown。

当前 CLI 仍只做 help/version/config 校验，不启动常驻服务。尚未实现 timerfd、signalfd、自动超时、动态插件、真实 AI/GPU、数据库、异步日志、benchmark；Phase 8 未开始。

## Phase 7G 最终交接（2026-08-03）

- 当前分支/HEAD/upstream：`phase/7-service-integration` / `a44b1272bf603a17724fa17c66d60ee0e18bb918`，均一致。
- warning 修复提交只修改 `tests/service/test_industrial_ai_service.cpp`，将参数名 `info` 改为 `test_info`，未改变测试逻辑。
- 最终 push [Linux CI run 30781932731](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30781932731)，workflow `Linux CI`，attempt 1，event `push`，conclusion `success`；Debug/Release checkout 均为该 SHA。
- Ubuntu 24.04.4 LTS、kernel 6.17.0-1020-azure、GCC 13.3.0、CMake 3.31.6；Debug/Release configure、build 和 CTest 均通过 `497/497`，warning 均为 0。
- Release version/config smoke 成功；`IndustrialAiServiceTest.ActiveHttpStopWaitsForDeferredCleanupBeforeJoiningTasks` Debug/Release 均通过。
- 当前状态：`PHASE_7_SERVICE_INTEGRATION_COMPLETED`。尚未执行 `ctest --repeat until-fail:50`。

Active stop 不保证触发请求返回 503；连接可能在响应生成前关闭，已开始发送的 response 不得截断。普通 Stopping 阶段 POST 仍返回 503。生命周期顺序：`TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`。

## Phase 8C-2 Configuration System 交接（2026-08-04）

- 分支：`phase/8-timer-infrastructure`；本轮不 commit/push。
- 状态：`PHASE_8C_2_CONFIGURATION_SYSTEM_IMPLEMENTED`，远端 Linux CI 待提交后验证。
- AppConfig 是 portable value；JSON parser 只在 config 层，core 不依赖 service。
- 新 schema 分组：service、server/reactor/tcp、http、runtime、tasks、plugins、task_api、logging。
- 旧顶层 `service_name/worker_threads/task_queue_capacity/log_level` 可加载；与对应新分组同时出现拒绝。
- schema 缺失为 1；未知字段、重复 key、非严格 integer、timeout 0、malformed 和大于 1 MiB 文件拒绝。
- RuntimeOptions 位于 service/runtime，复用 EventLoop/Timer/TCP/HTTP/ThreadPool/Task/Plugin/API 现有验证入口。
- Timer 容量层：TCP idle 为一层；HTTP header/body 互斥合计一层；容量不足为 ConfigError。
- `--config` 校验退出；Linux `--serve --config` 创建 EventLoop 和 IndustrialAiService，Service 自行接入 signalfd，随后 run。
- 不可破坏：TimerQueue/EventLoop stop、TCP/HTTP timeout、DeferredCleanup、TaskManager shutdown 和 signal owner 语义。
- 本地验证：Windows VS2022 Debug/Release `380/380`，WSL Ubuntu 24.04 GCC Debug/Release `596/596`；两平台 version/config smoke 成功，源码/测试 warning 0。
- 当前风险：远端 CI 尚未验证；WSL 直接构建当前 Windows 挂载工作区而非另一份 Linux clone；NTFS/WSL 时间戳可能产生非编译器的 clock-skew 提示。
