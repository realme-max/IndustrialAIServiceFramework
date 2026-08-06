# 阶段状态

## 当前结论

```text
PHASE_9B_2_APPLICATION_JOB_VALUE_INVARIANT_HARDENED
```

- 当前阶段：Phase 9B-2 Application Job Value Invariant Hardening（本地未提交）
- 基线：`d84557413fad50f26478698d8157bfb691d709ce`（Phase 9B-1 已提交并通过远程 CI）
- 实现状态：portable Job ID/snapshot、结构化 Repository contract 和线程安全有界内存 Repository 已实现；ID/Snapshot move 改为 copy-preserving 并在所有接受边界重验不变量；未改变 Phase 9B-1 状态矩阵
- Windows 验证：Visual Studio 2022 x64 Debug/Release 各注册 598 项，593 passed、5 explicitly skipped、0 failed；Application 65/65、Repository 40/40 passed
- WSL 验证：Ubuntu 24.04 / GCC 13.3.0 Debug/Release 各注册 826 项，825 passed、1 explicitly skipped、0 failed；Application 65/65、Repository 40/40 passed
- warning：项目源码与测试为 0；WSL 本地结果不冒充 GitHub Actions
- 日期：2026-08-06（Asia/Shanghai）
- 未实现：versioned HTTP Application API、持久化 Repository、Artifact I/O/Store、Worker Protocol、PTV2/WeldAgent adapter
- 下一步：Phase 9B-3 只实现 versioned HTTP/JSON Application API，不接入真实 worker

Phase 8A/8B/8C-1 已提供 timerfd、TCP/HTTP timeout 和 signalfd；Phase 8C-2 在不改变
这些生命周期语义的前提下增加一次性本地 JSON 配置和应用组合。Linux
`iaisf_server --serve --config` 会启动静态 Echo/MockVision、Task HTTP API 与监听；
既有动态插件和异步文件日志能力保持不变；Application Job Repository 尚未接入 Service，
任务自动 timeout 与热更新仍未实现。

Phase 7 新增 `iaisf_task_api`（跨平台）和 `iaisf_service`（Linux-only）。
Windows Debug/Release 均为 370/370，Task API 46/46；Linux Service 15 个 CTest
定义（参数展开 37 个 GoogleTest case）已定义
但尚未在真实 Linux 执行。CLI 仍不启动常驻服务。当前没有 commit、push 或 Phase 7
CI run，因此状态必须保持 validation blocked。

### Phase 7B 专项审计

- 原 stop 在 HttpServer active-batch stop 尚未真正完成时可能阻塞 join TaskManager；
  现改为 `StoppingHttp -> StoppingTasks -> Stopped`，由内嵌 DeferredCleanup 推进。
- `stopped()` 只在 HTTP stopped、Session/connection 表为空且 TaskManager worker
  全部 join 后为 true；重复 stop 幂等，Stopped 后不能 restart。
- 202 仅返回 task_id/status_url，不承诺 queued。
- ServiceOptions 在创建 worker/listener/Channel/route 前做精确跨层容量和 pool hard
  limit 校验；非法组合不产生运行时资源。
- Failed GET 返回安全 200；queue/repository/shutdown 使用 typed 503；TaskId 统一
  parser/formatter；router handler weak-own API，无强引用环。
- Windows Debug/Release 370/370，Release version/config smoke exit 0，项目 MSVC
  compiler warning 0。当前机器没有 bash/Linux，Linux scripts 未在本机执行。

## Git 状态

| 项目 | 结果 |
|---|---|
| 当前分支 | `phase/7-service-integration` |
| Phase 0 提交 | `5fbcec0 docs: complete phase 0 architecture design` |
| Phase 1 最终实现提交 | `63b30cffcbe3e621af33664721b3675a647bd1a1` |
| Phase 2 起始 HEAD / main / origin/main | `6065d91b277c07ed04e64b3f08034788965e6ac1` |
| Phase 2 Reactor 实现提交 | `f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f` |
| warning 修复 / 最终验证提交 | `4db8708a5121f8477d835addd0b16170a3e2054f` |
| Phase 2 文档封板 / Phase 3 基线 | `e14b23131eb917df5758a10a305c2c87997f24cf` |
| Phase 3 开始时 main / origin/main | `e14b23131eb917df5758a10a305c2c87997f24cf` |
| Phase 3 实现提交 | `0a45658d0e450dd9dfde052808a27ae92ad08881` |
| Phase 3 文档封板 / Phase 4 基线 / main / origin/main | `7096191ca8f7a3fe9e9acfb31ceba0a2c2fc3483` |
| Phase 4 HTTP 实现提交 | `9b87fdb8804ee37a8cf3b87a7b9193a3130b85d3` |
| Phase 4 测试修复 / 最终验证提交 | `0818ebf4f71366cc3cd2fe4e36e95fe667b687a5` |
| Phase 4 文档封板 / Phase 5 基线 / main / origin/main | `fe5b58446a14ebedf13978b0339f3ad0171f0ffa` |
| Phase 5 实现 / 最终验证提交 | `79d3d4e89feb71595dc67d820f9a5398dcc814d4` |
| Phase 5 最终 Linux CI | [run 30547126540](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540) |
| Phase 5 文档封板 / Phase 6 基线 / main / origin/main | `b8e7ded21ce9b78d0d59e18785a4912356eb5e15` |
| Phase 6 功能提交 | `66a606bb53bf8ed80b8efd6faf7c6529b5cd22d1` |
| Phase 6 warning 修复提交 | `853ccccca80cdc042b3d51eae52fe45566aa2b22` |
| Phase 7 基线 HEAD / main / origin/main | `9f88e0726a5db1abfe2ebe999de68aef41f317fb` |
| 当前分支 upstream | 未配置；Phase 7 尚未 commit/push |
| Phase 6 Linux 功能 CI | [run 30602538268](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30602538268) |
| Phase 6 最终零 warning Linux CI | [run 30604428624](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30604428624) |
| origin | `https://github.com/realme-max/IndustrialAIServiceFramework.git` |
| Phase 3 开始时工作区 | clean |
| Phase 6F 本轮 Git 操作 | 只更新九份阶段文档；未 commit/push |

本阶段没有 amend、reset、stash、rebase、merge、commit、push 或修改 origin。

## Phase 总览

| Phase | 名称 | 状态 | 验证结论 |
|---:|---|---|---|
| 0 | 只读调查与架构设计 | completed | 已提交为 `5fbcec0` |
| 1 | C++17 基础工程与公共基础设施 | completed | Windows 补充回归完成；Linux CI run `30508113122` success |
| 2 | Socket、epoll 与 EventLoop | completed | Debug/Release 均 87/87；44 个 Reactor 测试均实际执行 |
| 3 | TCP Transport Layer | completed | Debug/Release 138/138；Foundation 43、Reactor 45、TCP 50 |
| 4 | HTTP 协议与健康路由 | completed | Windows 127/127；Linux Debug/Release 239/239 |
| 5 | 线程池与任务系统 | completed | Windows Debug/Release 212/212；Linux Debug/Release 324/324，Task Runtime 85/85 |
| 6 | 插件系统 | completed | 最终 Linux Debug/Release 428/428、smoke pass；项目源码/测试 warning 0 |
| 7 | Application 组合与最小 HTTP Task API | implemented / Linux validation blocked | Windows API 回归通过；等待 Linux CI |
| 8 | 异步日志与配置扩展 | planned | 未开始 |
| 9 | 压力测试与工程完善 | planned | 未开始 |
| 10 | 真实工业视觉插件预留 | planned | 未开始，需用户明确批准 |

## Phase 1 已实现

### 构建

- CMake minimum 3.22，项目版本 0.1.0。
- C++17 且关闭 GNU extensions。
- target 级 include、link、compile features 和 warning。
- `configure_file` 生成 build-tree 版本头，不含时间、用户、机器、路径或 Git hash。
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，生成文件由 `.gitignore` 排除。
- 只声明 build-tree include 路径；没有尚未实现的 install/export 接口。

Targets：

```text
iaisf_core (STATIC)
  -> nlohmann_json::nlohmann_json

iaisf_server
  -> iaisf_core

iaisf_tests
  -> iaisf_core
  -> GTest::gtest
  -> GTest::gtest_main
```

CTest 还注册 `iaisf_server_version` 和 `iaisf_server_example_config`。

### 依赖

- nlohmann/json `v3.11.3`
- GoogleTest `v1.15.2`
- 默认固定 tag FetchContent
- `IAISF_USE_SYSTEM_DEPS=ON` 时使用 `find_package`
- `IAISF_BUILD_TESTS=OFF` 时不获取 GoogleTest
- 第三方源码仅存在忽略的 build 目录，没有加入 Git

### Error/Result

- `ErrorCode`：InvalidArgument、ConfigError、IoError、InternalError。
- 稳定小写字符串及未知值 fallback。
- Error 保留公开可变字段，非空不是类型级永久不变量。
- 项目生产路径使用 `make_error`；构造、工厂和 Result failure 边界归一化空 message。
- `Result<T>` 使用 `std::variant`，支持 move-only 和非默认构造类型。
- `Result<void>` 表达无返回值操作。
- API 误用抛 `std::logic_error`；普通失败不使用异常控制流。

### AppConfig

结构：

```text
service.name
runtime.worker_threads
runtime.task_queue_capacity
logging.level
```

默认值：

- service name：IndustrialAIServiceFramework
- worker threads：硬件并发度夹在 1—256
- task queue capacity：1024
- log level：info

规则：

- service name 最大 128 bytes，非空、非全空白、无控制字符；
- worker threads 1—256；
- task queue capacity 1—1,000,000；
- log level 严格小写 `trace/debug/info/warn/error`；
- 缺失分组/字段使用默认值；
- 所有未知顶层和组内字段返回 ConfigError；
- JSON 语法、类型和值错误返回 ConfigError；
- 文件不存在、非普通文件、无法打开/读取返回 IoError；
- 错误不输出完整配置或不必要的绝对路径。

### 同步日志

- `ILogger` 最小虚接口。
- `ConsoleLogger` 注入 `std::ostream&`，不拥有流。
- 支持五级阈值和运行时更新。
- mutex 保护阈值和完整行写出。
- UTC 毫秒时间、level、component、message。
- 换行和控制字符被清洗。
- 没有后台线程、队列、文件、轮转或压缩。

### Application

- `--version`：固定输出并返回 0，不加载配置。
- `--help`：输出 usage 并返回 0。
- `--config <path>`：加载/验证配置、初始化同步 logger、输出验证日志并退出。
- 无参数：输出版本和 Phase 1 提示并退出。
- 未知、冲突、缺失/多余参数：输出错误和 usage，返回 2。
- 配置失败：返回 1，不抛出预期异常。
- `main.cpp` 只收集参数、构造 Application、返回退出码并处理不可预期异常。

### Phase 1B 审计结论

- Error：公开字段允许构造后被清空；改为准确记录“工厂和 Result 边界归一化”，并增加清空后进入 Result 的测试。
- Result：通过编译期断言固定 value/error 的 `&`、`const&`、`&&` 返回类别；move-only 和 rvalue move 测试通过。
- 版本头：只生成到 build tree，源码树没有 `version.hpp`；应用和测试均 include `iaisf/version.hpp`。
- CMake：移除未实现的 `INSTALL_INTERFACE`；未发现全局 include/link/definitions、硬编码 toolchain 或本机路径。
- AppConfig：补充 float/string/null/bool、超大整数和 UTF-8 byte 上限测试；JSON 库异常在配置加载边界转换为 ConfigError。
- Logger：Linux 使用 `gmtime_r`、Windows 使用 `gmtime_s`；注入 stream 必须比 logger 生命周期长，且不能在 logger 外并发直写。
- Application：help/version 不读配置，config 错误返回 1，参数错误返回 2，不启动网络或线程。
- CTest：仍为 41 项 unit 和 2 项 smoke；路径不依赖 CTest 工作目录。审计发现原进程内序号在并行 CTest 进程间可能重名，现改为原子创建唯一系统临时目录并由 RAII 删除；Windows Debug `ctest -j 8` 复验为 43/43。
- Shell：三个脚本通过 `bash -n`；本机未安装 shellcheck，未自动安装。

## Phase 2 已实现

### 构建边界

- Linux-only 静态库 `iaisf_net`，alias 为 `iaisf::net`；PUBLIC 链接 `iaisf::core`，PRIVATE 链接 `Threads::Threads`。
- `IAISF_BUILD_LINUX_NETWORK` 在 Linux 默认 ON、其他平台默认 OFF；非 Linux 显式开启会在 CMake configure 阶段明确失败。
- Windows 保持只构建 Phase 1 targets 和 43 个既有 CTest，不解析 Linux 系统头。
- Linux 构建脚本显式传递 `-DIAISF_BUILD_LINUX_NETWORK=ON`；CI 两个 job 均设置 15 分钟超时。

### Reactor 原语

- `UniqueFd`：move-only fd RAII，`-1` 表示无效；提供 `get/valid/release/reset/swap`，析构只尝试一次 `close`。
- `Socket`：创建 `AF_INET/SOCK_STREAM` IPv4 TCP fd，同时设置 nonblocking 与 close-on-exec；支持 `SO_REUSEADDR`、write-half shutdown 和所有权转移。
- `Channel`：不拥有 fd，绑定一个 `EventLoop`；`RDHUP` 属于 read-side，`HUP|IN` 仍执行 read，只有 HUP 且没有 read-side 事件时才直接 close；随后按 error、read、write 分派。
- `EpollPoller`：独占 `EPOLL_CLOEXEC` epoll fd，提供 add/update/remove/poll；使用有界事件数组和 ET，不使用 ONESHOT。
- `EventLoop`：单 owner 线程、`Created/Running/Stopping/Stopped` 状态；持有 Poller 和 wakeup Channel。Created 可预入队，run 前 stop 清空队列并直接 Stopped；Stopping/Stopped 拒绝新提交。
- `eventfd`：使用 `EFD_NONBLOCK | EFD_CLOEXEC`；写端允许唤醒合并，读端持续 drain 到 `EAGAIN`。
- `queue_in_loop`：线程安全有界回调队列；空回调返回 `InvalidArgument`，队列满返回 `ResourceExhausted`。状态/容量检查、入队、唤醒与失败回滚在同一互斥区内，failure 不残留本次回调。
- Channel 注册期地址稳定且 fd 有效，析构前必须 remove；Debug 析构断言检查。active 批次内直接 remove 被拒绝，必须投递延迟操作。
- 回调异常不会退出 loop；一个 Channel 抛出后停止其本次剩余回调，但后续 active Channel 继续。注入的非 owning `ILogger` 负责记录，logger 自身失败通过 `logger_failure_count` 可观察。
- `ErrorCode` 新增稳定值 `SystemError`、`InvalidState`、`ResourceExhausted`。

### 明确未实现的网络能力

本阶段没有 `bind/listen/accept/connect`、Acceptor、TcpConnection、TcpServer、echo server、连接读写缓冲或协议解析。`iaisf_server` 没有接入 EventLoop，运行行为仍是 Phase 1 CLI。

### Phase 2 测试与验证

- 44 个 Linux-only Reactor 测试：UniqueFd 8、Socket 5、Channel 7、EpollPoller 7、EventLoop 17；Debug/Release 均实际执行 44/44。
- 测试只使用 `eventfd` 和 `socketpair`，不监听端口、不依赖外部网络；等待均有有限超时，CTest timeout 为 10 秒。
- Phase 2B 新增 HUP/RDHUP 顺序、fd 复用、stop-before-run、Stopping 拒绝、多生产者容量竞争、唤醒失败回滚、active 批次延迟移除、Channel 异常边界和多次 eventfd drain 覆盖。
- Windows VS2022 Debug configure/build/CTest：pass，43/43；Release clean build/CTest：pass，43/43。
- Windows Release `--version` 输出 `IndustrialAIServiceFramework 0.1.0`，示例配置 smoke 退出码为 0。
- 非 Linux 显式开启网络选项的负向 configure：按预期失败，诊断为仅支持 Linux epoll/eventfd。
- 三个 shell 脚本通过 `bash -n`，workflow YAML 可解析；禁止实现项扫描与 `git diff --check` 通过。
- Phase 2 Linux Debug/Release configure、build 均成功；CTest 均为 87/87、0 failed，其中 Reactor 44/44。
- Release 独立 smoke 成功；最终 Debug/Release 项目源码 warning 为 0、项目测试 warning 为 0。
- 未安装 cppcheck、clang-tidy、actionlint、shellcheck；未自动安装。Sanitizer 未执行。

### Phase 2 首次功能 Linux CI

[run 30514521602](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30514521602)
对应 Reactor 实现提交 `f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f`。Debug/Release
configure、build、87/87 CTest 和 smoke 均通过，但 Release 测试构建存在 2 条
`-Wunused-result` warning，因此它只作为首次功能通过记录，不是最终零 warning 证据。

### Phase 2 最终零 warning Linux CI

| 项目 | 实际记录 |
|---|---|
| workflow | `Linux CI` |
| run ID / attempt | `30516007475` / `1` |
| run URL | [https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475) |
| event | `push` |
| branch | `phase/2-reactor-core` |
| head SHA | `4db8708a5121f8477d835addd0b16170a3e2054f` |
| checkout SHA | Debug/Release 均为 `4db8708a5121f8477d835addd0b16170a3e2054f` |
| status / conclusion | `completed` / `success` |
| runner | 两个 job 均为 GitHub-hosted `ubuntu-24.04` |
| OS / kernel | Ubuntu 24.04.4 LTS / 6.17.0-1020-azure x86_64 |
| GCC / CMake | 13.3.0 / 3.31.6 |
| Debug | configure pass；build pass；CTest 87/87，0 failed |
| Release | configure pass；build pass；CTest 87/87，0 failed |
| Reactor 参与度 | `iaisf_net`、`iaisf_net_tests` 均构建；Debug/Release 各执行 Reactor 44/44 |
| smoke | version 和示例配置均 pass |
| 非成功 job/step | 0；没有 failed、cancelled、skipped 或 neutral |
| `continue-on-error` | 0 |
| 项目源码 warning | Debug 0；Release 0 |
| 项目测试 warning | Debug 0；Release 0；两处 `-Wunused-result` 已消失 |

Release smoke 实际输出：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T05:14:04.588Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

完整日志中只有 `git init` 默认分支提示含有单词 “warning”；它不是项目源码、
项目测试或第三方依赖的编译 warning。

## Phase 3 已完成

### 构建边界

```text
iaisf_tcp (STATIC, Linux only)
  PUBLIC -> iaisf::net

iaisf_tcp_tests (Linux only)
  PRIVATE -> iaisf::tcp
  PRIVATE -> GTest::gtest / GTest::gtest_main / Threads::Threads
```

`iaisf::net` 继续包含底层 Socket/Reactor 和 Ipv4Endpoint 实现；`iaisf::tcp`
包含 Buffer、Acceptor、TcpConnection、TcpServer。Windows
`IAISF_BUILD_LINUX_NETWORK=OFF` 时两个 TCP target 都不存在，MSVC 不解析 Linux
TCP 源码或 epoll/eventfd/accept4 头。

### 实现契约

- `Ipv4Endpoint`：IPv4 only、numeric parse、无 DNS、host-order port、清零
  `sockaddr_in`、稳定字符串和 round-trip。
- `Buffer`：maximum > 0、initial ≤ maximum；reader/writer index、prependable、
  ensure/compact、retrieve 不 memmove、append 必要时前部复用或有界增长；超限与
  `bad_alloc` 映射 `ResourceExhausted`。
- `Socket`：bind/listen/local endpoint、`TCP_NODELAY`、`SO_ERROR`、
  `accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)`；EINTR/ECONNABORTED 重试，
  EAGAIN 返回空 optional。
- `Acceptor`：owner-thread-only，拥有 Socket 与 Channel，ET accept 到 EAGAIN；
  EMFILE/ENFILE/其他错误记录并结束当前 batch，不 busy loop；callback 异常由
  accepted Socket RAII 回收并继续 accept；active read callback 内 stop 延迟移除 Channel。
- `TcpServerOptions`：使用有符号 factory 输入拒绝负数，验证 backlog、连接数、
  input/output initial/max/high-water、可选 `SO_SNDBUF` 和 64 MiB hard bound，
  不静默 clamp；默认不覆盖系统 socket buffer。
- `TcpConnection`：server table 是主要 shared owner；Channel callback 捕获 weak；
  `Connecting → Connected → Disconnecting → Disconnected`；close callback 至多一次。
- read：recv 到 EAGAIN，EINTR 重试，input 超限 fail-closed；message callback
  负责 retrieve，异常只关闭当前连接。
- write：owner 线程 `send()` 先整包容量预留、登记 `EPOLLOUT`、再一次性追加；
  返回 success 表示整包已接受，failure 不发送或缓存本次前缀。可写回调使用
  `MSG_NOSIGNAL` 循环，部分写/EAGAIN 后缀仍在 Buffer；排空即停用。
- high-water：只在 below→at/above 跨越时通知，降到 below 后重武装；通知异常隔离，
  不冒充完整限流。
- peer EOF：停止读取但仍交付本轮已读数据一次；应用负责 retrieve，部分或完全未
  消费的数据不会重复回调，销毁时丢弃；输出非空先排空。force-close 用于 server
  stop，本地主动 graceful shutdown 无 timerfd timeout。
- `TcpServer`：有界 `unordered_map<uint64_t, shared_ptr<TcpConnection>>`；达到
  max 时 accepted Socket 由 RAII 立即关闭；连接先入表再 established。close 进入
  预分配待移除向量并幂等登记 intrusive cleanup 节点，不依赖普通 pending queue；
  顺序为 remove Channel → close Socket → erase table → 最后 shared_ptr 释放。
- `TcpServer::stop()` owner-thread-only、幂等、停止 accept、force-close 现有连接且
  永久禁止 restart。非 active 调用同步清空表；active callback 内调用在 batch 后
  完成，`stopped()` 是完成屏障。started server 未完成 stop 就析构会终止进程。

### 测试定义

| 文件 | `TEST` 定义 |
|---|---:|
| Buffer | 13 |
| Ipv4Endpoint | 4 |
| Acceptor/Socket server operations | 7 |
| TcpConnection | 6 |
| TcpServer integration | 20 |
| 合计 | 50 |

覆盖 port 0、burst accept、binary/fragment/128 KiB Echo、8 客户端、最大连接数、
input/output hard maximum、small `SO_SNDBUF` 的动态 EPOLLOUT/high-water 二次跨越、
peer half-close、RST、callback exception、close once、active batch 后延迟 remove、
最后强引用释放、满普通队列清理、多连接 active stop、EOF 部分/不消费、析构契约和
连接表清理。另新增 1 项 Reactor intrusive cleanup 测试；Linux Debug/Release 均
实际执行基础 43、Reactor 45、TCP 50，合计 138/138。测试无固定端口、无
外网、无 fixed sleep、无 detached thread，CTest timeout 20 秒。

### 本地实际验证

| 检查 | 结果 |
|---|---|
| Windows VS2022 configure，network OFF | pass |
| Windows Debug clean build / CTest | pass / 43/43 |
| Windows Release clean build / CTest | pass / 43/43 |
| Release `--version` | exit 0；`IndustrialAIServiceFramework 0.1.0` |
| Release example config | exit 0；包含 `configuration validated` |
| Windows 项目 C++ warning | 0 |
| 非 Linux network ON 负向 configure | 预期失败，明确要求 Linux epoll/eventfd |
| 三个 shell 脚本 `bash -n` | pass |
| workflow YAML parse | pass；jobs 为 linux-debug/linux-release |
| `git diff --check` | pass |

本机仍无 Linux/WSL；没有执行本地 Linux build。Phase 3 结论来自验证当时与
Phase 3 实现提交、checkout 和 upstream 完全一致的 GitHub Actions push run，不以
Phase 2 run 或源码定义数代替。当前 HEAD 已是后续的 Phase 4 基线。

### 最终 Linux CI

| 项目 | 结果 |
|---|---|
| workflow / event / attempt | `Linux CI` / `push` / 1 |
| run | [30524686201](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30524686201) |
| branch / commit | `phase/3-tcp-transport` / `0a45658d0e450dd9dfde052808a27ae92ad08881` |
| head / checkout / local / upstream（验证当时） | 四者完全一致 |
| runner / OS | `ubuntu-24.04` / Ubuntu 24.04.4 LTS |
| GCC / CMake | 13.3.0 / 3.31.6 |
| Debug | configure/build success；138/138 CTest，0 failed |
| Release | configure/build success；138/138 CTest，0 failed |
| 实际测试分布 | Foundation 43/43；Reactor 45/45；TCP 50/50 |
| TCP targets | `iaisf_tcp`、`iaisf_tcp_tests` 在两个 job 中均实际构建 |
| Release smoke | version/config 均 success |
| warning | 项目源码 0；项目测试 0 |
| run conclusion | `success`；无 failed/cancelled/skipped/neutral/timeout |
| failure masking | 无 `continue-on-error` |

Release smoke 实际输出摘要：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T07:58:10.003Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

本轮 Windows clean build 的项目 C++ warning 为 0。MSBuild 的 vcpkg applocal
仍打印缺少 `pwsh.exe` 的非致命辅助诊断，但 configure/build 退出码、43/43 CTest
和两项 smoke 均成功；该诊断不属于 Linux/TCP 结果。

## Phase 4 HTTP Protocol 已完成

### Targets 与能力

```text
iaisf_http_core (portable)
  PUBLIC -> iaisf::core

iaisf_http (Linux only)
  PUBLIC -> iaisf::http_core
  PUBLIC -> iaisf::tcp
```

Core 实现 HttpStatus/HttpLimits/Request/Response/Parser/Router/built-ins。Adapter
实现 HttpSession/HttpServer。HTTP 只支持 1.1、origin-form、strict CRLF、
Content-Length、默认 keep-alive 和有限顺序 pipeline；不支持 chunked、TE、
Upgrade、Expect、HTTP/1.0/2、percent decode、动态参数或流式 body。

### 容量、安全与所有权

- HttpLimits 的 10 项值均为正、有硬上限和跨字段校验，有符号 factory 拒绝负数；
  request/header 单行限制包含 CRLF，header total 包含终止空行。
- Parser 每次返回 consumed count；所有重复 header name 都返回 400；Host 恰好一个，
  CL+TE 和歧义长度返回 400，CL 溢出/超 body 返回 413，TE 返回 501，Expect 返回
  417。组合错误优先级固定，不依赖无序容器迭代。
- `Connection` 只匹配 comma-separated 完整 token；空 token、非法 token 拒绝。
- Response 自动写 Content-Length/Connection，拒绝 framing header 覆盖和 CRLF
  注入；header count/line/head total 预检包含自动 framing，失败不返回部分响应；
  错误响应为固定 plain text、close，不回显请求或内部错误。
- Router exact method+path，freeze 后只读；404/405 可保持连接，handler Error/
  标准或未知异常变成 closed 500。
- HttpServer owns TcpServer、frozen Router、Session table，不拥有 EventLoop/Logger；
  TcpServer owns connection，Session only holds weak connection。server callbacks 捕获
  weak server，无 raw `this`。
- 每轮最多处理 `max_requests_per_dispatch`；每个 Session 同时至多一个 continuation，
  weak/state 检查阻止 stop/断连后重入，普通 queue 失败即 force-close。请求 close/
  错误后丢弃后续 pipeline。
- HTTP close 通过 `close_after_write()` 写尽后主动全关闭，不等待客户端 EOF；
  `shutdown()` 仍为传输层半关闭。连接与 Session 清理使用 active-batch-safe 的内部
  `DeferredCleanup`，不受普通 pending queue 容量影响。

### 实际 Windows 结果

| 配置 | Build | Foundation | HTTP Core | CTest | smoke | 项目 warning |
|---|---|---:|---:|---:|---|---:|
| VS2022 Debug | pass | 43/43 | 84/84 | 127/127 | CTest 内 version/config | 0 |
| VS2022 Release | pass | 43/43 | 84/84 | 127/127 | 独立 version/config exit 0 | 0 |

最终 Debug/Release `--clean-first --parallel` 均成功。项目源码/测试没有 warning。
已知缺失 `pwsh.exe` 诊断仍来自 VS/vcpkg applocal，不是项目编译 warning。

### Linux 最终验证

| 项目 | 结果 |
|---|---|
| workflow | Linux CI |
| run | [30539245789](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30539245789) |
| attempt / event / conclusion | 1 / push / success |
| branch | `phase/4-http-protocol` |
| head / checkout / local / upstream | `0818ebf4f71366cc3cd2fe4e36e95fe667b687a5` |
| runner / OS | `ubuntu-24.04` / Ubuntu 24.04.4 LTS |
| GCC / CMake | 13.3.0 / 3.31.6 |
| Debug | configure/build pass；239/239，0 failed |
| Release | configure/build pass；239/239，0 failed |
| 分项 | Foundation 43；Reactor 45；TCP 51；HTTP Core 84；HTTP Integration 16 |
| HTTP targets | `iaisf_http_core`、`iaisf_http_core_tests`、`iaisf_http`、`iaisf_http_tests` 均在两个 job 构建 |
| Release smoke | version/config pass |
| 项目源码/测试 warning | 0 / 0 |
| 非成功或掩盖 | failed/cancelled/skipped/neutral/timeout/continue-on-error 均为 0 |

首次 [run 30537924856](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30537924856)
对应实现提交 `9b87fdb8804ee37a8cf3b87a7b9193a3130b85d3`，Debug/Release
均为 237/238。唯一失败测试的 32 字节响应 Header 单行 fixture 无法容纳含 CRLF
共 41 字节的固定错误 `Content-Type` 行；Parser 已得到 400，错误响应预检失败后
按设计 fail-closed。修复提交 `0818ebf4f71366cc3cd2fe4e36e95fe667b687a5`
调整该测试容量并新增 exact-limit portable 回归。最终 run 中原失败测试
`HttpServerTest.RejectsFramingAmbiguitiesAndConfiguredLimits` 在 Debug/Release
均实际通过。

## 环境调查

### Windows

| 项目 | 结果 |
|---|---|
| OS | Windows 10 家庭中文版 10.0.19045 x64 |
| CPU | Intel Core i5-12400，12 logical processors |
| RAM | 约 15.75 GiB |
| Git | 2.53.0.windows.1 |
| CMake/CTest | 4.1.0 |
| Visual Studio C++ | MSVC 19.38.33130.0，VS 2022 |
| PATH g++ | MinGW-w64 GCC 5.3.0 i686，未用于验收 |

### Linux/WSL

- `wsl.exe --status`：exit 50。
- `wsl.exe --list --verbose`：exit 1，只返回安装/用法信息。
- 本机无可运行发行版，无法在本机复现 Linux 构建。
- Docker、Podman 均未发现。
- 未自动安装 WSL、发行版、Docker 或系统包。

### Phase 1 GitHub Actions Linux

| 项目 | 实际记录 |
|---|---|
| runner label | `ubuntu-24.04`（Debug、Release） |
| OS | Ubuntu 24.04.4 LTS（Debug `Record environment`） |
| kernel | 6.17.0-1020-azure x86_64（Debug `uname -a`） |
| GCC | 13.3.0（Debug、Release configure） |
| CMake | 3.31.6（Debug `Record environment`） |

## Phase 1 历史 Linux 验收证据

### Linux Debug

| 项目 | 结果 |
|---|---|
| configure | pass |
| build | pass |
| CTest | pass |
| 测试总数/通过/失败 | 43 / 43 / 0 |
| 项目编译 warning | 0 |

### Linux Release

| 项目 | 结果 |
|---|---|
| configure | pass |
| build | pass |
| CTest | pass |
| 测试总数/通过/失败 | 43 / 43 / 0 |
| 项目编译 warning | 0 |

### Linux smoke

| 项目 | 结果 |
|---|---|
| `--version` | pass；输出 `IndustrialAIServiceFramework 0.1.0` |
| example config | pass；INFO 日志包含 `configuration validated for service IndustrialAIServiceFramework` |
| workflow step | `Smoke Release` conclusion = `success` |

### Windows/MSVC 补充结果

| 配置 | Configure | Build | CTest |
|---|---|---|---|
| Debug | pass | pass | 43/43 pass，0 failed |
| Release | pass（同一 multi-config tree） | pass | 43/43 pass，0 failed |

上述结果为 2026-07-30 Phase 1B 干净回归。最终项目自身 C++ 编译 warning 为 0。Visual Studio 的本机 vcpkg applocal 集成在 executable target 后尝试调用缺失的 `pwsh.exe`，产生非致命诊断；退出码、产物和 CTest 均成功。这不是项目 CMake 发起的依赖模式，也不是 Linux 结果。

Windows 首次配置使用默认 FetchContent 模式并访问 GitHub，固定版本的 nlohmann/json 与 GoogleTest 获取成功；没有启用系统依赖模式。第三方源码和产物只位于被忽略的 `build/windows-vs2022/_deps`。

### Phase 1 GitHub Actions Linux CI

- workflow：`Linux CI`（`.github/workflows/linux-ci.yml`）
- run ID：`30508113122`
- run URL：[https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122)
- event / attempt：`push` / `1`
- commit：`63b30cffcbe3e621af33664721b3675a647bd1a1`
- branch：`phase/1-foundation`
- runner：两个 job 均为 GitHub-hosted `ubuntu-24.04`
- `Linux Debug`：job 与环境记录、configure/build、CTest 步骤均为 `success`
- `Linux Release`：job 与 configure/build、CTest、smoke 步骤均为 `success`
- workflow status / conclusion：`completed` / `success`
- 权限：`contents: read`
- 日志检查：未出现项目编译 warning；Release job 未单独重复打印 CMake 版本

手工 Windows Release smoke：

| 命令 | 退出码 | 输出摘要 |
|---|---:|---|
| `iaisf_server.exe --version` | 0 | `IndustrialAIServiceFramework 0.1.0` |
| `iaisf_server.exe --config config/iaisf.example.json` | 0 | INFO 日志包含 `configuration validated` |
| `iaisf_server.exe` | 0 | 版本和“不启动网络”提示 |
| `iaisf_server.exe --unknown` | 2 | argument error 和 usage |

## 实际命令

主要命令：

```powershell
git status --short
git branch --show-current
git log -1 --oneline
git remote -v
git switch -c phase/1-foundation
wsl.exe --status
wsl.exe --list --verbose

cmake -S . -B build/windows-vs2022 `
  -G "Visual Studio 17 2022" -A x64 `
  -DIAISF_BUILD_TESTS=ON `
  -DIAISF_USE_SYSTEM_DEPS=OFF
cmake --build build/windows-vs2022 --config Debug --clean-first --parallel
ctest --test-dir build/windows-vs2022 -C Debug --output-on-failure
cmake --build build/windows-vs2022 --config Release --clean-first --parallel
ctest --test-dir build/windows-vs2022 -C Release --output-on-failure

.\build\windows-vs2022\Release\iaisf_server.exe --version
.\build\windows-vs2022\Release\iaisf_server.exe `
  --config .\config\iaisf.example.json
```

```bash
bash -n \
  scripts/build_linux.sh \
  scripts/test_linux.sh \
  scripts/smoke_linux.sh
```

正式 Linux 脚本已由最终 Phase 2 GitHub Actions run `30516007475` 在提交
`4db8708a5121f8477d835addd0b16170a3e2054f` 上执行。本机仍没有可运行的
Linux/WSL 环境，因此本地未复现这次 CI。

## Phase 5 Task Runtime 实现与验证

### Targets 与文件边界

- `iaisf_task` / `iaisf::task` 是跨平台静态库；PUBLIC 依赖 `iaisf::core`、
  `Threads::Threads`、`nlohmann_json::nlohmann_json`。
- `iaisf_task_tests` 在 Windows 与 Linux 均构建，Phase 5B 后定义 85 项 `unit;task` 测试。
- Linux workflow 的 Debug/Release 显式 target 列表均加入 `iaisf_task` 和
  `iaisf_task_tests`，同时保留已有 HTTP targets、完整 CTest 和 Release smoke。
- 未修改 AppConfig JSON schema；现有 `worker_threads`/`task_queue_capacity` 没有
  在 CLI 启动 pool。

### 线程池与任务契约

- `ThreadPoolOptions`：`worker_threads` 与 `queue_capacity` 都必须大于 0；
  hard maximum 分别为 256 和 1,000,000，不 clamp。
- `BoundedThreadPool`：固定 worker、有界 FIFO、condition variable 等待、
  non-blocking `try_submit`。队列满为 ResourceExhausted，停止后为 InvalidState。
- worker 在队列锁外执行；标准与未知异常均被计数和隔离。shutdown 停止 admission、
  drain、join，幂等；并发 caller 只有一个执行 join，其他等待 Stopped；join 后清空
  thread 对象。worker self-shutdown 在修改状态前失败，不 restart/resize/detach。
- `TaskId` 在 Repository mutex 内单调生成；默认无效，文本为 `task-` +
  最少 16 位十进制。不声明跨进程/重启唯一；rollback/erase 后不复用，耗尽后永久
  ResourceExhausted，不回绕。
- `TaskState` 只有 Queued、Running、Succeeded、Failed、TimedOut。合法转换只有
  Queued→Running 与 Running→三个终态。
- `TaskRepository` 是 first-terminal-wins 唯一裁决者；首个终态为 Applied，
  其余为 AlreadyTerminal。不存在与非法转换分别是 NotFound/InvalidState。
- `TaskLimits` 在保存前限制 operation、input/result 序列化 bytes、error message
  bytes 和 Repository 记录数；超限结果不部分写入。
- `TaskExecutor` 在仓库锁外调用可并发 `TaskHandler`；异常文本不进入 Snapshot，
  logger 异常不逃逸，超限/不可序列化 result 转 Failed。AlreadyTerminal 和
  TimedOut erase 后的 NotFound late completion 均有计数。
- `TaskManager::submit` 在 admission mutex 下增加 in-flight 计数后，于锁外 create
  Queued、构造闭包并 `try_submit`；RAII guard 在所有返回路径减计数，队列/分配失败
  只 rollback 仍为 Queued 的记录。
- Manager shutdown 先把 accepting 设为 false，等待 in-flight submission 归零，
  再调用 Pool drain/join；并发 caller 幂等，handler 执行期间不持 lifecycle lock。
- Repository 不自动驱逐、不做 TTL 或持久化；只有 `erase_terminal` 释放容量。
- 非协作 handler 会延迟 shutdown；Phase 5 不提供强杀、取消 token 或自动 timeout。

### Windows 实际结果

| 配置 | Foundation | HTTP Core | Task Runtime | 合计 | 失败 |
|---|---:|---:|---:|---:|---:|
| VS2022 x64 Debug | 43 | 84 | 85 | 212 | 0 |
| VS2022 x64 Release | 43 | 84 | 85 | 212 | 0 |

Release version smoke：

```text
IndustrialAIServiceFramework 0.1.0
```

Release example-config smoke：

```text
2026-07-30T13:23:45.738Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

项目 MSVC 源码和测试编译 warning 为 0。现有 Visual Studio/vcpkg applocal 阶段仍
打印非致命 `pwsh.exe` 诊断，但构建 exit 0、CTest 全通过；它不是编译器 warning。

### Linux 最终结论

| 项目 | 结果 |
|---|---|
| workflow / event / attempt | `Linux CI` / `push` / 1 |
| run | [30547126540](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540) |
| conclusion / branch | `success` / `phase/5-task-runtime` |
| head / Debug checkout / Release checkout | `79d3d4e89feb71595dc67d820f9a5398dcc814d4` |
| runner / OS | `ubuntu-24.04` / Ubuntu 24.04.4 LTS |
| kernel | `6.17.0-1020-azure` |
| compiler / CMake | GCC 13.3.0 / CMake 3.31.6 |
| Linux Debug | configure/build success；324/324 CTest，0 failed |
| Linux Release | configure/build success；324/324 CTest，0 failed |
| 分项 | Foundation 43、Reactor 45、TCP 51、HTTP Core 84、HTTP Integration 16、Task Runtime 85 |
| task targets | Debug/Release 均实际构建 `iaisf_task`、`iaisf_task_tests` |
| warning | 项目源码 0；项目测试 0 |

Release smoke：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-30T13:31:28.474Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

两个 job 和全部可见步骤均为 success；没有 failed、cancelled、skipped、neutral、
timeout 或 `continue-on-error`。当前宿主仍没有本地 Linux/WSL；上述结论来自可追溯
CI，不宣称本机执行了 Linux build。

## Phase 6 Static Plugin System 实现与验证

### Targets 与边界

- `iaisf_plugin` / `iaisf::plugin` 是跨平台静态库，PUBLIC 依赖 task/core/json，
  不依赖 net/tcp/http。
- `iaisf_plugin_tests` 是跨平台 `unit;plugin` target，共 92 项。
- Task Runtime 新增可选 TaskValidator 与通用 JSON 结构边界，Task tests 由 85 增至 97。
- Linux workflow Debug/Release 均显式构建 `iaisf_plugin` 和 `iaisf_plugin_tests`，
  保留完整 CTest 与 Release smoke。

### 实现契约

- PluginLimits 对 registry/metadata/error、input/output bytes、JSON depth/elements/string
  和 capabilities 设经 factory 验证的硬上限，不 clamp。
- PluginMetadata 是独立值，operation/capability 使用 canonical lowercase ASCII，
  capability 列表有数量、字节和去重约束。
- PluginManager 强持有 const plugin shared handle，Configuring 阶段只允许注册和
  freeze；find/list/validate/execute 均返回 InvalidState 且不调用插件。
  register/freeze 在同一 mutex 上线性化，Frozen 后永久只读；不持 registry mutex
  调用 plugin。
- 注册只调用一次 metadata；null、duplicate、capacity、非法 metadata 或异常失败
  不改变 registry。list 返回稳定排序副本。
- TaskManager 在 in-flight submission 内、TaskId 分配前执行通用校验和可选 validator；
  validator 在 Manager/Repository mutex 外运行，shutdown 等待其完成。
- 通用 JSON 审计有界遍历结构，并以 nlohmann compact dump 的精确 UTF-8 输出字节
  计数；引号、反斜杠、控制字符和 object key 转义均计入，超过上限立即中止且不创建
  `dump()` 整文档副本。discarded、非法 UTF-8、non-finite 和全部容量边界有测试。
- `PluginManager::execute` 的直接调用也不能绕过输入/plugin validation；成功输出在
  Repository 前统一校验，超限不会留下半结果。
- Adapter 只接受 Frozen Manager；validator/handler closure 只强持有只读 Manager，
  不持有 Adapter 或 TaskManager。worker 对 owned request snapshot 再次校验；结果
  变化固定 InternalError 且不调用 execute。TaskManager/TaskExecutor 释放 closure 后，
  Manager 与插件可析构，不存在 shared ownership cycle。
- unknown operation 返回 NotFound 且不创建 TaskId/record/queue entry。
- validation/execution 异常和内部错误固定泛化，不把 `what()`、路径或原始输入写入
  TaskSnapshot；TaskLimits 继续控制 result/error。
- Echo 对合法请求返回 `payload` 的原值独立副本，不增加 wrapper 字段；Echo 和
  MockVision 无共享可变状态。MockVision 永远 `mock: true`，不读取文件、
  不运行模型或 GPU，结果不代表准确率、性能或 production readiness。

### Windows 实际结果

| 配置 | Foundation | HTTP Core | Task Runtime | Plugin System | CTest | 失败 |
|---|---:|---:|---:|---:|---:|---:|
| VS2022 x64 Debug | 43 | 84 | 97 | 92 | 316/316 | 0 |
| VS2022 x64 Release | 43 | 84 | 97 | 92 | 316/316 | 0 |

Release smoke：

```text
IndustrialAIServiceFramework 0.1.0
2026-07-31T03:29:24.782Z [INFO] [Application] configuration validated for service IndustrialAIServiceFramework
```

Debug/Release clean build 均成功，项目源码和测试编译 warning 为 0。Visual Studio
环境的既有 vcpkg applocal `pwsh.exe` 缺失诊断非致命且不是编译器 warning。
新增并发测试使用 promise/future 屏障和有限 wait_for；没有 fixed sleep 或 detached
thread。

### 当前 Linux 结论

Phase 6 功能提交 `66a606bb53bf8ed80b8efd6faf7c6529b5cd22d1` 对应首次
[Linux CI run 30602538268](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30602538268)，
workflow `Linux CI`，push event，attempt 1，completed/success。Debug 和 Release
checkout SHA、本地 HEAD 与 upstream 一致。runner 为 `ubuntu-24.04`，Ubuntu
24.04.4 LTS，kernel `6.17.0-1020-azure`，GCC 13.3.0，CMake 3.31.6。

| Job | Configure/build | Foundation | Reactor | TCP | HTTP Core | HTTP Integration | Task | Plugin | CTest | Smoke |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| Linux Debug | success | 43 | 45 | 51 | 84 | 16 | 97 | 92 | 428/428 | N/A |
| Linux Release | success | 43 | 45 | 51 | 84 | 16 | 97 | 92 | 428/428 | version/config success |

`iaisf_plugin`、`iaisf_plugin_tests` 与规定的并发、freeze/register 竞态、输入快照、
JSON 转义字节限制、Echo 任意 JSON 原值及 MockVision `mock: true` 测试均实际执行。
没有 failed、cancelled、skipped、neutral、timeout 或 `continue-on-error`。

该首次 run 的 Debug 和 Release 各有 3 条项目源码 warning（全部
`-Wsign-conversion`）及 3 条项目测试 warning（全部
`-Wmissing-field-initializers`），因此只作为功能通过历史记录。

warning 修复提交 `853ccccca80cdc042b3d51eae52fe45566aa2b22` 对应最终
[Linux CI run 30604428624](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30604428624)，
workflow `Linux CI`，push event，attempt 1。head SHA、Debug checkout、Release
checkout、本地 HEAD 与 upstream 完全一致。两个 job 的 configure/build 均成功，
Debug/Release 各实际通过 428/428，分项仍为 Foundation 43、Reactor 45、TCP 51、
HTTP Core 84、HTTP Integration 16、Task Runtime 97、Plugin System 92。
`iaisf_plugin` 和 `iaisf_plugin_tests` 均实际构建，Release version/config smoke
成功；完整日志中项目源码 warning 0、项目测试 warning 0。workflow、两个 job 和
所有步骤均 success，无 failed、cancelled、skipped、neutral、timeout 或
`continue-on-error`。最终状态：

```text
PHASE_6_PLUGIN_SYSTEM_COMPLETED
```

## 参考工程保护

任务开始基线：

| 文件数 | 字节数 | 聚合 SHA-256 |
|---:|---:|---|
| 62 | 59,240,225 | `83AE7E469DEA30C860DEFD4D26CB313B7B3C87EFCD9387414741E152EE46CF27` |

Phase 5 开始、交付前和 Phase 5C 封板前均复核为相同值；Phase 6 开始、交付前及
Phase 6D 文档更新及 Phase 6F 封板前后也再次一致。
参考工程未被构建、格式化或添加到 Git。

## 未实现

- CLI `--serve`、生产常驻模式和外部配置到 ServiceOptions 的组合
- 动态 `.so`/DLL、插件发现、热加载/卸载和进程隔离
- timerfd/signalfd、自动任务/连接超时和取消
- 异步日志、文件日志和轮转
- 数据库、用户、HTML、TLS、Docker、部署和 Release 发布
- benchmark、性能数据、真实 AI
- sanitizer 和 `/proc` fd 长时间稳定性结果

`worker_threads` 和 `task_queue_capacity` 仍只是 CLI/AppConfig 中经过校验的配置
字段；Phase 7 ServiceOptions 尚未接入 AppConfig。当前 CLI 不创建 worker、队列、
插件 registry 或 listener。

## 当前阻塞与风险

- Phase 1 没有剩余验收阻塞。
- Phase 2 没有剩余验收阻塞。
- Phase 3 没有剩余封板阻塞；本机仍无 Linux/WSL，当前 Linux 结论来自可追溯的 GitHub Actions run。
- Phase 4、Phase 5 和 Phase 6 均已完成最终 Linux 封板。
- Phase 7 代码已实现但尚无对应提交的 Linux CI，不能封板。
- 默认 FetchContent 首次 Linux 配置需要 GitHub 网络和有效 CA 证书。
- 系统依赖模式已设计但未在已安装 Linux 包环境验证。
- Windows/NTFS 工作区不能可靠表达新 shell 脚本的 POSIX executable bit；workflow 会在运行时执行 `chmod +x scripts/*.sh`，人工 Linux 使用前仍应确认权限。
- Visual Studio 本机 vcpkg applocal 集成的非致命 `pwsh.exe` 诊断不影响当前测试，但应与 Linux 结果分开记录。

## Phase 8 建议入口

Phase 7 Linux 封板后才可规划 timerfd 连接/任务 timeout、signalfd 停止接入、
ServiceOptions/AppConfig 组合与有界异步日志。动态插件、GPU/真实 AI、数据库和
benchmark 仍不在下一阶段。

## 建议 commit

本轮未执行 commit。建议：

```text
feat: integrate task HTTP service
```

## Phase 7E 历史封板审计（2026-08-03）

历史结论：这是一次真实成功的 Linux 功能 CI，但未满足 warning=0 封板门槛；最终状态以 Phase 7G 记录为准。

- workflow：Linux CI；run [30779555703](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30779555703)，attempt 1，event `pull_request`，conclusion `success`。
- Phase 7 修复提交/HEAD：`1cc332b9d9e02ae78ec9e43455d36ffe939f73e2`；job checkout merge SHA：`466b80f742457da1fa67aea2859b770e361dcbe4`。
- Ubuntu 24.04.4 LTS（runner `ubuntu-24.04`，kernel `6.17.0-1020-azure`）、GCC 13.3.0、CMake 3.31.6。
- Debug：configure/build pass，CTest `497/497`；Release：configure/build pass，CTest `497/497`；两者 0 failed。
- Release version/config smoke：pass，输出版本 `0.1.0` 和 configuration validated，exit 0。
- `IndustrialAiServiceTest.ActiveHttpStopWaitsForDeferredCleanupBeforeJoiningTasks`：Debug/Release 均 pass。
- 项目源码 warning：未观察到；项目测试 warning：`tests/service/test_industrial_ai_service.cpp:1041:51` 的 `-Wshadow`，每 job 3 次、共 6 条日志记录。
- workflow 没有 failed/cancelled/skipped/neutral/timeout，未使用 `continue-on-error`；尚未执行 `ctest --repeat until-fail:50`。

Active HTTP stop 契约：触发 stop 的当前请求不保证 503，连接可由 cleanup 关闭；普通 Stopping 阶段新 POST 仍为 503。顺序为 `TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`。

未实现：常驻 `--serve` CLI、timerfd/signalfd、自动任务超时、动态插件、真实 AI/GPU、数据库、异步日志和 benchmark。Phase 8 尚未开始。

## Phase 7G 最终封板（2026-08-03）

结论：`PHASE_7_SERVICE_INTEGRATION_COMPLETED`。

- warning 修复提交：`a44b1272bf603a17724fa17c66d60ee0e18bb918`，仅修改 `tests/service/test_industrial_ai_service.cpp` 的参数名。
- 最终 push workflow：[Linux CI run 30781932731](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30781932731)，attempt 1，event `push`，conclusion `success`；head/checkout/local HEAD 均为该 SHA。
- 环境：runner `ubuntu-24.04`，Ubuntu 24.04.4 LTS，kernel 6.17.0-1020-azure，GCC 13.3.0，CMake 3.31.6。
- Debug/Release configure、build 成功，CTest 均 `497/497`，0 failed；项目源码 warning=0，项目测试 warning=0。
- Release version/config smoke 成功；ActiveHttpStop 测试 Debug/Release 均通过。
- 尚未执行 `ctest --repeat until-fail:50`。

Active stop 语义：触发 stop 的当前请求不保证 503，连接可能在响应生成前关闭，已开始发送的 response 不得截断；普通 Stopping 阶段 POST 仍为 503。生命周期顺序为 `TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`。

当前未实现：常驻 `--serve` CLI、timerfd/signalfd、自动任务超时、动态插件、真实 AI/GPU、数据库、异步日志和 benchmark。Phase 8 尚未开始。

## Phase 8C-2 Configuration System（2026-08-04）

状态：`PHASE_8C_2_CONFIGURATION_SYSTEM_IMPLEMENTED`。

- Portable AppConfig、严格 JSON schema v1、legacy 顶层配置兼容已实现。
- RuntimeOptions 复用现有 Options 校验，并完成 EventLoop/Timer/TCP/HTTP/Task/Plugin/API 映射。
- Linux `--serve --config` 已组合 Service、静态插件、HTTP/TCP 和 signalfd 完整停止链路。
- Windows VS2022 Debug/Release `380/380`，本地 WSL Ubuntu 24.04 GCC Debug/Release `596/596`；两平台 version/config smoke 成功。
- 配置专项实际包括 AppConfig 22 项、RuntimeOptions 5 项和 Linux `--serve`/SIGTERM 进程集成 1 项；需求列出的 16 类场景均有覆盖。
- 项目源码 warning 0、项目测试 warning 0；当前未提交实现尚无 GitHub Actions CI 证据。
- 未实现：热更新、YAML、环境变量覆盖、动态插件、异步文件日志、数据库、真实 AI/GPU 和 benchmark。
## Phase 8G-4D Dynamic Plugin Configuration and Service Integration

状态：`PHASE_8G_4D_DYNAMIC_PLUGIN_CONFIGURATION_IMPLEMENTED`

- `plugins.runtime` schema 支持 startup-only dynamic loading、相对安全路径、平台库选择、模块上限和受限 JSON config。
- `RuntimeOptions` 将 portable `AppConfig` 转换为已选平台库的 `DynamicPluginOptions`；旧静态插件配置保持兼容。
- Service 创建事务执行静态注册、动态加载、`register_dynamic`、freeze 和 HTTP 启动；任意失败均返回错误并通过 RAII 回滚。
- 新增 `plugin_dynamic_modules_loaded` gauge 与 `plugin_dynamic_load_failures_total` counter，均为无 label、best effort。
- Plugin diagnostics 增加动态加载开关和已加载模块数量，不暴露 root、library 或 config。
- 测试 fixture 构建为平台原生 MODULE（Windows DLL / Linux SO），由 CTest 自动构建依赖。

本地验证：WSL Ubuntu Debug/Release `752/752`；Windows VS2022 Debug/Release 各 524 个测试（520 passed、4 个既有环境相关 skipped）。项目源码与测试 warning 为 0，`git diff --check` 通过。本轮尚未取得新的 GitHub Actions run，不将本地结果描述为远程 CI 证据。

当前未实现：热加载/卸载、运行期插件管理、远程下载、进程隔离、真实 AI/GPU。下一步需先提交并在 Linux CI 验证本阶段提交。

## Phase 8G-4E: final dynamic-plugin hardening audit

Status: `PHASE_8G_FINAL_DYNAMIC_PLUGIN_HARDENED` (local, uncommitted).

The audit verified Config → RuntimeOptions → Loader → DynamicModule → C ABI →
Adapter → PluginRuntime → TaskManager/HTTP ownership, lease-protected module
lifetime, startup transaction rollback, and shutdown ordering. Create,
initialize, execute, shutdown, destroy and native-unload failures are bounded
and isolated; unload failures increment `plugin_dynamic_unload_failures_total`
and still clear the native handle.

Fixed dynamic metrics are `plugin_dynamic_modules_loaded`,
`plugin_dynamic_load_failures_total` and
`plugin_dynamic_unload_failures_total`. Diagnostics exposes only bounded state,
counts, origin and module-id; paths, root, config, handles, payloads and
exception text are excluded.

Validation performed on the final source:

| Environment | Result | Notes |
|---|---:|---|
| WSL Ubuntu 24.04 Debug | 761/761 | one explicit permission-capability skip, zero failures |
| WSL Ubuntu 24.04 Release | 761/761 | one explicit permission-capability skip, zero failures |
| Windows VS2022 Debug | 528 passed / 533 registered | five explicit environment skips, zero failures |
| Windows VS2022 Release | 528 passed / 533 registered | five explicit environment skips, zero failures |

Project C/C++ source and test warnings were zero. The Windows build's
non-fatal `pwsh.exe` lookup messages come from the local Visual Studio
applocal helper, not the project compiler. The existing Linux workflow already
builds plugin, fixture, loader/adapter tests and Service targets; no new remote
run was created for this uncommitted worktree. ASan/UBSan were not run because
no sanitizer configuration is enabled locally. No hot reload, remote plugin,
process isolation or sandbox is included.
