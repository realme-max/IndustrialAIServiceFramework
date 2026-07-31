# 架构设计

## 1. 文档状态

- 项目：IndustrialAIServiceFramework
- 阶段：Phase 7 Service Integration and Task HTTP API
- 日期：2026-07-31
- 状态：`PHASE_7_SERVICE_INTEGRATION_IMPLEMENTED_LINUX_VALIDATION_BLOCKED`；跨平台 API 已通过 Windows 回归，Linux-only Service 仍等待真实 Linux CI
- 目标平台：Linux x86_64，C++17

本文同时记录已实现的 Phase 1 基础设施、Phase 2 Reactor、Phase 3 TCP、Phase 4 HTTP、Phase 5 Task Runtime 与 Phase 6 静态插件系统，以及后续目标边界。只有明确列入已实现边界的类才是当前能力。

### 1.1 Phase 1 已实现边界

已实现：

- `ErrorCode`、`Error`、`Result<T>`、`Result<void>`
- `AppConfig` 的 JSON 加载、默认值和严格校验
- `LogLevel`、`ILogger`、同步 `ConsoleLogger`
- 最小 `Application` 和 CLI
- CMake targets、版本生成和自动化测试

未实现：

- 本文后续描述的 Acceptor、TcpConnection、TcpServer、HTTP、ThreadPool、Task、Plugin 和 Timer 类
- 后台日志线程、异步队列、文件日志或日志轮转

### 1.2 Phase 2 已实现边界

Linux-only `iaisf_net` 当前实现：

- `UniqueFd` 独占 fd；不可复制，可移动，析构不抛异常且不重试 `close(EINTR)`。
- `Socket` 只创建 IPv4 TCP fd、设置 `SO_REUSEADDR` 和执行 write-half shutdown，不 bind/listen/accept/connect。
- `Channel` 不拥有 fd，也不直接调用 `epoll_ctl`；fd 和所属 EventLoop 必须比 Channel 活得更久，注册期地址不可变化，析构前必须移除。
- `EpollPoller` 独占 epoll fd，使用固定上限事件数组，不拥有注册的 Channel。
- `EventLoop` 在构造线程运行；`update_channel/remove_channel/run` 仅允许所属线程调用。
- `queue_in_loop/stop` 可跨线程调用，通过 nonblocking、close-on-exec 的 `eventfd` 唤醒。
- 待执行回调队列按元素数硬限制；空回调返回 `InvalidArgument`，满队列返回 `ResourceExhausted`。
- active Channel 批次分派期间直接 remove 被拒绝；应用回调仍使用有界 `queue_in_loop`，Phase 3 框架生命周期对象则使用独立的 intrusive `DeferredCleanup` 节点在整批处理后清理。
- Channel 回调和待执行回调的异常被记录并隔离。一个 Channel 抛出后停止其本次剩余回调，但后续 active Channel 继续。Logger 不归 EventLoop 所有，必须活得更久。

Phase 2 直接由 EventLoop 持有 `EpollPoller`，没有为了未来替换性创建空壳 `Poller` 基类。

### 1.3 Phase 2 验证与下一阶段边界

Reactor 实现提交 `f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f` 的首次功能
[GitHub Actions Linux CI run 30514521602](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30514521602)
已通过，但 Release 测试构建记录 2 条 `-Wunused-result` warning。warning 修复提交
`4db8708a5121f8477d835addd0b16170a3e2054f` 的最终
[Linux CI run 30516007475](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475)
在 Ubuntu 24.04.4 LTS、GCC 13.3.0、CMake 3.31.6 上验证。Debug 和 Release
均完成 configure/build，并各有 87/87 CTest 通过；44 个 Reactor 测试在两个配置中
都实际执行。Release smoke 的版本和示例配置检查成功，项目源码和测试 warning 均为
0；没有 failed、cancelled、skipped、neutral 或 `continue-on-error`。

Phase 2 封板时只建议 Phase 3 实现连接层：`Buffer`、`Acceptor`、`TcpConnection`、`TcpServer`，
ET accept/read/write 到 `EAGAIN`、动态启停 `EPOLLOUT`、输出缓冲高水位、
连接建立/半关闭/关闭生命周期和原始字节 Echo 集成测试。Phase 3 不包含 HTTP parser、
HttpRouter、ThreadPool、TaskRepository、TaskManager、PluginManager、timerfd、
signalfd、异步日志、AI 推理或 benchmark。

### 1.4 Phase 3 已实现边界

Phase 3 实现提交新增 Linux-only `iaisf_tcp` / `iaisf::tcp`，PUBLIC 依赖
`iaisf::net`。实现边界为：

- `Ipv4Endpoint` 是 numeric IPv4 + host-order `uint16_t` port 值类型；使用
  `inet_pton/inet_ntop`，不查询 DNS，不支持 IPv6。
- `Buffer` 是 owner-thread-only 有界二进制缓冲；retrieve 只移动 reader index，
  append 需要空间时才 compact/grow，增长前使用 `length > max - readable`。
- `Socket` 增加 bind/listen/local endpoint、`TCP_NODELAY`、`SO_ERROR` 和
  `accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)`。
- `Acceptor` 拥有监听 Socket 和非 owning Channel，以 ET 循环 accept 到
  `EAGAIN`；监听 Channel 的声明/销毁顺序保证先 remove/destroy Channel，再关闭 fd。
- `TcpConnection` 使用 server table 的 `shared_ptr` 作为主要所有者；Channel
  回调捕获 `weak_ptr`，构造期间不调用 `shared_from_this`。
- `TcpServer` 拥有 Acceptor 和有界连接表，不拥有 EventLoop；close callback
  只把强连接引用加入预分配待移除向量，并幂等登记内嵌 cleanup 节点。真正流程是
  remove Channel → 关闭 Socket → erase table → 释放最后一个 `shared_ptr`。
- read/write 都循环到 `EAGAIN`；发送使用 `MSG_NOSIGNAL`。`send()` 先为整包预留
  Buffer 并登记 `EPOLLOUT`，再一次性追加；可写回调排空到 `EAGAIN`，排空后停用。
- input/output hard maximum 独立强制；output high-water 只在 below→at/above
  跨越时通知，降到阈值以下后重武装，不等同于完整应用层限流。
- peer EOF 停止后续读取，但本轮已读取字节仍交给 message callback；有待发送
  数据时先排空，再进入延迟关闭。
- `TcpServerOptions` 的可选 `socket_send_buffer_bytes` 是通用、受硬上限验证的
  `SO_SNDBUF` 调优项；默认不覆盖系统值，测试用它构造确定性 backpressure。

Phase 3 实现提交 `0a45658d0e450dd9dfde052808a27ae92ad08881` 已由
[Linux CI run 30524686201](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30524686201)
验证。Ubuntu 24.04.4 LTS、GCC 13.3.0、CMake 3.31.6 的 Debug/Release 均成功
configure/build，均为 138/138 CTest 通过，其中 Foundation 43、Reactor 45、TCP 50。
两个配置均实际构建 `iaisf_tcp` 和 `iaisf_tcp_tests`；Release 版本/示例配置 smoke
成功，项目源码和测试 warning 均为 0。run、两个 job 和全部步骤均为 success，
没有 failed、cancelled、skipped、neutral 或 `continue-on-error`。

### 1.5 Phase 4 已实现边界

Phase 4 新增两个清晰 target：

- 可移植 `iaisf_http_core` / `iaisf::http_core`，只依赖 `iaisf::core`，包含
  HttpStatus、HttpLimits、HttpRequest/Response、增量 Parser、Router 和 built-ins。
- Linux-only `iaisf_http` / `iaisf::http`，依赖 `iaisf::http_core` 与
  `iaisf::tcp`，包含 HttpSession 和 HttpServer。

Windows Debug/Release 已各实际执行 Foundation 43 + HTTP Core 84，共 127/127
CTest。最终 [Linux CI run 30539245789](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30539245789)
对应修复提交 `0818ebf4f71366cc3cd2fe4e36e95fe667b687a5`；Debug/Release configure/build
和 239/239 CTest 均成功，其中 HTTP Core 84/84、HTTP Integration 16/16。四个 HTTP
target 均实际构建，Release CLI smoke 成功，项目源码和测试 warning 为 0。
`iaisf_server` 仍没有进入常驻模式。

### 1.6 Phase 5 已实现边界

Phase 5 新增跨平台 `iaisf_task` / `iaisf::task`，PUBLIC 依赖 `iaisf::core`、
`Threads::Threads` 和 nlohmann/json。它不依赖 Reactor、TCP 或 HTTP target。

- `BoundedThreadPool` 在工厂成功返回前启动固定数量 worker；有界 FIFO 的
  `try_submit` 不等待空位。队列满为 `ResourceExhausted`，shutdown 后为
  `InvalidState`。
- Pool 状态严格为 Running→ShuttingDown→Stopped；`try_submit` 和 shutdown
  在同一 mutex 下线性化。首个 shutdown caller 执行 drain/join，其他并发 caller
  等待 Stopped；join 后清空 thread 对象。worker self-shutdown 在修改状态前返回
  `InvalidState`，之后仍可由外部线程正常 shutdown。
- `TaskId` 是 `uint64_t` 强类型，仓库锁内单调分配；字符串最小宽度 16 位十进制，
  不包含随机数、时间、机器信息，也不宣称跨进程或重启唯一。rollback 和 erase
  不复用 ID；分配到最大值后永久返回 `ResourceExhausted`，不回绕。
- `TaskLimits` 以 bytes 校验 operation/error，并对 input/result 校验 JSON depth、
  total elements、string/key bytes 与紧凑 UTF-8 serialized bytes；序列化计数不保存
  第二份整文档。discarded、non-finite、非法 UTF-8、分配和长度错误转为 `Error`。
  error 超限使用固定长度 `#` 泛化，不保留敏感前缀。
- `TaskRepository` 是唯一状态裁决者，持有有界内存记录和独立快照。它不自动驱逐、
  不做 TTL/持久化；只有显式 `erase_terminal` 释放终态容量。
- `TaskExecutor` 在仓库锁外调用注入式 `TaskHandler`。handler 标准/未知异常映射为
  泛化 Failed，logger 异常被计数和隔离，超限或不可序列化结果转为 Failed。
  TimedOut 后的晚到完成遇到 AlreadyTerminal，或 TimedOut 已删除后遇到 NotFound，
  都作为 late completion 计数并丢弃。
- `TaskManager::submit` 先在 admission mutex 下检查 `accepting` 并增加
  `in_flight_submissions`，随后在锁外完成 validate/create/try_submit/rollback。
  RAII guard 在所有返回路径减少计数；失败提交不保留 Queued 记录。
- shutdown 的 Manager 线性化点是把 `accepting` 设为 false；它先等待
  `in_flight_submissions == 0`，再调用 Pool drain/join。多个 submitter 不会串行
  执行 handler，多个 shutdown caller 由 Pool 状态机收敛。

Windows Debug/Release 均实际执行 Foundation 43、HTTP Core 84、Task Runtime 85，
合计 212/212。Phase 5 实现提交 `79d3d4e89feb71595dc67d820f9a5398dcc814d4`
已由最终 [Linux CI run 30547126540](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540)
验证；Ubuntu 24.04.4 LTS、GCC 13.3.0、CMake 3.31.6 的 Debug/Release configure、
build 均成功，CTest 均为 324/324，其中 Task Runtime 85/85。两个配置均实际构建
`iaisf_task` 与 `iaisf_task_tests`，Release version/config smoke 成功，项目源码
和测试编译 warning 为 0。

### 1.7 Phase 6 已实现边界

Phase 6 新增跨平台 `iaisf_plugin` / `iaisf::plugin`，PUBLIC 依赖
`iaisf::task`、`iaisf::core` 和 nlohmann/json，不依赖 Reactor、TCP 或 HTTP。

- `PluginLimits` 是 factory 验证后的不可变安全边界；所有有符号输入先拒绝非正数，
  再转换为 `size_t`。除 registry/metadata/error 外，还限制输入/输出紧凑序列化字节、
  JSON 深度、总 value 节点数、单字符串/对象键和 capability 数量/字节。
- `PluginMetadata` 是独立值；operation/capability 只接受小写 ASCII `a-z0-9._-`，
  拒绝首尾点和连续空分段；capabilities 必须唯一，全部文本拒绝 NUL/控制字符和非法 UTF-8。
- `IAlgorithmPlugin` 只暴露 metadata、无 I/O 快速 `validate_input` 和可并发
  `execute`；接口中没有网络、HTTP、TaskRepository、文件系统、取消或 GPU 类型。
- `PluginManager` 强持有 `shared_ptr<const IAlgorithmPlugin>`，禁止复制/移动，
  operation 是唯一键。注册只调用一次 metadata 并保存副本；null、重复、容量、
  非法 metadata 或异常失败都不改变 registry。
- Manager 状态不可逆地从 Configuring 进入 Frozen。freeze 幂等；注册与 freeze 在同一
  registry mutex 上线性化。Configuring 只允许 register/freeze，list/lookup/validate/
  execute 均返回 InvalidState 且不调用插件；Frozen 后注册快速拒绝，其他操作可并发。
  调用插件前在锁内复制 shared handle，释放锁后才进入插件代码，因而阻塞插件不阻塞
  其他 metadata lookup。
- 通用 JSON 审计先做有界结构遍历，再由 nlohmann/json serializer 向 counting stream
  输出，从而精确计算与 `dump()` 相同的紧凑 JSON bytes；对象/数组标点、键、引号、
  转义和 UTF-8 实际字节均计入，不保留整份序列化文本，并在首个超限字节立即中止。
  discarded、non-JSON binary、非法 UTF-8 和非有限浮点数 fail-closed。
- 公共 `PluginManager::execute` 自带输入容量和 plugin validation，直接 C++ 调用不能
  绕过校验；插件成功输出在返回或进入 TaskRepository 前统一执行相同结构/字节校验。
- `TaskManager` 新增可选 `TaskValidator` 且保留旧 handler-only API。submit 在
  in-flight admission 后依次完成通用 TaskLimits 校验、锁外 validator、create_queued、
  non-blocking try_submit 与失败 rollback；validator failure 不分配 TaskId。成功 submit
  前 Repository 与 worker closure 已各自拥有 request 数据，调用方后续修改或销毁
  operation/input 不影响已接收任务。
- `PluginTaskAdapter` 只接受 Frozen Manager。它生成的 validator/handler 只捕获
  `shared_ptr<const PluginManager>`，不保活 Adapter 或 TaskManager；Manager 再强持有
  plugin，因此所有权图无反向边和强引用环。提交前校验一次，worker 对已拥有的
  TaskRequest 快照再做一次 plugin contract 校验；结果变化返回固定 InternalError 且
  不调用 execute。
- validation 的安全 InvalidArgument/ResourceExhausted 受字节上限控制；validation/
  execute 的标准或未知异常以及 execute 返回的任意插件 Error 均泛化为固定
  InternalError。输出容量错误保留 ResourceExhausted，异常 `what()`、路径和原始输入
  不写入 TaskSnapshot。
- `EchoPlugin` 无状态、无 I/O，成功时直接复制返回 payload，不额外写入 operation；
  `MockVisionPlugin` 无状态且始终 `mock: true`，只按 width/height/threshold 生成
  确定性 JSON，不读取图片/点云、不运行模型或 GPU。

Windows Debug/Release clean build 均为 316/316：Foundation 43、HTTP Core 84、
Task Runtime 97、Plugin System 92，项目源码和测试 warning 为 0。

Phase 6 功能提交 `66a606bb53bf8ed80b8efd6faf7c6529b5cd22d1` 的首次功能
[Linux CI run 30602538268](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30602538268)
在 Ubuntu 24.04.4 LTS、kernel `6.17.0-1020-azure`、GCC 13.3.0 和 CMake 3.31.6
上完成 Debug/Release configure/build、428/428 CTest 与 Release smoke。实际分项为
Foundation 43、Reactor 45、TCP 51、HTTP Core 84、HTTP Integration 16、Task Runtime
97、Plugin System 92；两个 plugin target 和要求的专项测试均实际构建/执行。
然而 Debug 与 Release 各有 3 条项目源码 warning 和 3 条项目测试 warning，故该 run
只能证明功能通过，不能完成零 warning 封板。

warning 修复提交 `853ccccca80cdc042b3d51eae52fe45566aa2b22` 的最终
[Linux CI run 30604428624](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30604428624)
在相同 Ubuntu/GCC/CMake 基线上完成 Debug/Release configure、build 和 428/428
CTest；`iaisf_plugin`、`iaisf_plugin_tests`、Task Runtime 97 项及 Plugin System
92 项均实际构建/执行，Release version/config smoke 成功。完整日志中项目源码和
测试 warning 均为 0，两个 job 与所有步骤均 success，因此 Phase 6 完成封板。

## 2. 调查结果与设计来源

### 2.1 当前工作区

Phase 0 开始时，工作区根目录只有只读参考目录 `TinyWebServer_reference`，没有项目代码、CMake 工程、Git 仓库或用户未提交改动。`PTV2-WeldSeg-Deployment` 和 `weld_agent` 未出现在当前工作区，因此未读取、未修改。

### 2.2 参考工程只读观察

参考目录包含 62 个文件，调查时聚合 SHA-256 为：

```text
83AE7E469DEA30C860DEFD4D26CB313B7B3C87EFCD9387414741E152EE46CF27
```

观察到的主要模块：

- 根级 `WebServer` 负责配置、监听、epoll 循环、连接数组、定时器、日志、线程池和数据库初始化。
- `http_conn` 同时承担 Socket I/O、HTTP 解析、静态文件响应和 MySQL 登录相关逻辑。
- 模板线程池使用 pthread、信号量、裸请求指针和分离线程。
- 定时器采用信号与管道唤醒，容器为有序双向链表。
- 日志采用单例、阻塞队列和后台线程。
- 工程包含 MySQL 连接池、HTML/图片/视频站点资源和 webbench。
- 监听与连接支持 LT/ET 组合，也包含 Reactor/模拟 Proactor 选择。

可借鉴的只是通用思想：非阻塞 Socket、epoll 事件分派、ET 下读到 `EAGAIN`、部分写处理、HTTP 增量状态机、工作队列、空闲连接超时和异步日志。

### 2.3 自主改造原则

不复用参考工程目录、类名、数据结构或实现代码。本项目以工业 AI **任务服务化** 为核心重新划分边界：

- 网络对象不理解 HTTP 路由，更不理解工业业务。
- HTTP 会话不持有数据库、插件或视觉资源。
- 任务调度使用通用闭包和统一 Task 模型，不把连接对象直接扔给工作线程。
- 插件管理器是显式实例，禁止隐式全局注册表。
- 所有 fd 使用 RAII；连接对象按 EventLoop 线程归属管理。
- 使用最小堆与 `timerfd`，不使用 `alarm`/`SIGALRM`。
- 不包含登录、HTML、MySQL、任意文件服务或 CGI。
- 设计从第一天支持自动化单元/集成测试和容量上限。

## 3. 设计目标与非目标

### 3.1 目标

- Linux 上的单 Reactor、epoll ET、非阻塞网络服务。
- HTTP/1.1 JSON API；未来可增加长度前缀 TCP JSON 协议。
- 有界线程池和任务队列，耗时计算与网络 I/O 隔离。
- 统一任务状态、结果、超时和错误模型。
- 静态注册插件，隔离插件异常。
- 可配置、可记录、可测试、可优雅停止。
- 为未来视觉、机器人和编排插件提供边界，而不把它们耦合进核心。

### 3.2 非目标

- HTTPS、HTTP/2、WebSocket、chunked、multipart。
- 多 Reactor、每核 EventLoop、零拷贝。
- 动态 `.so` 加载或稳定二进制 ABI。
- 数据库、用户登录、HTML 静态站点。
- 真实 TensorRT/PCL/PointNet++、机器人控制或 Agent。
- 分布式任务队列、任务持久化、跨进程恢复。

## 4. 分层与依赖规则

```mermaid
flowchart TB
    Client["Client"]
    Network["network<br/>Socket / Channel / EventLoop / TCP"]
    Http["http<br/>Parser / Session / Response"]
    Router["core<br/>Router / API handlers"]
    Task["task<br/>TaskManager / Repository / Executor"]
    Concurrency["task runtime<br/>BoundedThreadPool"]
    Plugin["plugin<br/>PluginManager / PluginTaskAdapter / IAlgorithmPlugin"]
    Examples["plugin implementations<br/>Echo / MockVision"]
    Timer["timer<br/>TimerQueue / timerfd"]
    Logging["logging<br/>Logger"]
    Config["config<br/>typed configuration"]

    Client --> Network
    Network --> Http
    Http --> Router
    Router --> Task
    Task --> Concurrency
    Plugin --> Task
    Plugin --> Examples
    Timer --> Network
    Timer --> Task
    Config -. injects .-> Network
    Config -. injects .-> Task
    Config -. injects .-> Plugin
    Logging -. observed by .-> Network
    Logging -. observed by .-> Task
    Logging -. observed by .-> Plugin
```

依赖规则：

1. `network` 不依赖 `http`、`task` 或 `plugin`。
2. `http` 通过字节流与回调适配 `network`，不执行任务。
3. `core` 负责装配和路由，可以依赖上层服务接口。
4. `task` 只定义通用 TaskValidator/TaskHandler，不依赖 plugin。
5. `plugin` 依赖 task 完成适配并拥有内置 Echo/MockVision；task/core/network 不反向依赖具体插件。
6. `logging`、`config` 提供实例化服务，不使用全局可变单例。
7. 跨层错误使用统一 `Error`，不得静默吞异常。

## 5. 推荐目录

```text
include/iaisf/
  core/          Application, Error, Result, Router, ServiceContext
  net/           UniqueFd, InetAddress, Buffer, Channel, EpollPoller,
                 EventLoop, Acceptor, TcpServer, TcpConnection
  http/          HttpRequest, HttpResponse, HttpParser, HttpSession, HttpRouter
  task/          TaskId, TaskState, TaskLimits, BoundedThreadPool,
                 TaskRepository, TaskManager, TaskExecutor
  plugin/        IAlgorithmPlugin, PluginLimits, PluginMetadata,
                 PluginManager, PluginTaskAdapter, EchoPlugin, MockVisionPlugin
  timer/         TimerId, TimerQueue
  logging/       LogRecord, ILogger, Logger
  config/        ServerConfig, ConfigLoader
src/             与公共模块对应的实现；main.cpp 只做组合根和进程退出码
plugins/
  echo/          无状态回显插件
  mock_vision/   显式 mock 工业视觉示例
tests/
  unit/          纯模块测试
  integration/   启动真实服务的端到端测试
examples/        示例客户端和请求脚本
scripts/         构建、启动、smoke、sanitizer 辅助脚本
```

## 6. 核心类清单

| 类/概念 | 职责 | 线程安全与生命周期 |
|---|---|---|
| `UniqueFd` | 独占 fd，移动语义，析构关闭 | 不共享；所有权明确 |
| `Ipv4Endpoint` | numeric IPv4、host-order port 和 `sockaddr_in` 转换 | 无资源值对象 |
| `Buffer` | 有界连续字节缓冲、reader/writer 游标与前部复用 | 仅归属 EventLoop |
| `Channel` | fd、关注事件、回调，不拥有 fd | 仅 EventLoop 修改 |
| `EpollPoller` | RAII 管理 epoll fd 和 `epoll_ctl/wait` | 仅 EventLoop 使用 |
| `EventLoop` | epoll 轮询、eventfd 唤醒、跨线程待执行函数、owner-thread internal cleanup lane | 本体单线程；`queue_in_loop()`/`stop()` 可跨线程 |
| `Acceptor` | 创建监听 fd、accept 循环、连接上限前置检查 | EventLoop 线程 |
| `TcpServer` | 持有连接表、创建和移除连接、停止接收 | EventLoop 线程 |
| `TcpConnection` | 连接状态、读写缓冲、半关闭和回压 | EventLoop 线程；外部只持弱引用/投递操作 |
| `HttpParser` | 增量解析 request line/headers/body | 每连接独占，无共享 |
| `HttpSession` | 把 TCP 字节转换为 HTTP 请求/响应 | EventLoop 线程，不执行耗时业务 |
| `HttpRouter` | 方法 + 路径模板匹配 | 启动后只读 |
| `BoundedThreadPool` | 固定工作线程、有界 FIFO、drain/join | 公共方法线程安全；worker self-shutdown 拒绝 |
| `TaskRequest/Snapshot` | 通用 JSON 输入与独立状态快照 | 值对象；不含网络引用 |
| `TaskRepository` | 有界内存存储、查询、合法状态转换、终态清理 | 内部互斥，公共方法线程安全 |
| `TaskManager` | 通用校验、可选 validator、创建任务、排队、查询、超时协调 | 公共 API 线程安全；validator/handler 可并发 |
| `TaskExecutor` | 工作线程中的 handler 调用边界、异常隔离和结果提交 | 不创建线程；handler 必须并发安全 |
| `IAlgorithmPlugin` | metadata、快速输入校验和算法执行契约 | validate/execute 可在同一实例同时运行；插件作者自行保证线程安全 |
| `PluginManager` | 显式静态注册、freeze、查找、校验、异常隔离 | Configuring 写；Frozen 后并发只读/调用且不持锁进入插件 |
| `PluginTaskAdapter` | 生成 TaskValidator/TaskHandler 并二次校验 | shared-owned；闭包保活 manager/plugin，无环 |
| `TimerQueue` | `timerfd` + 最小堆、取消/更新、执行过期回调 | EventLoop 归属；跨线程操作通过 `queue_in_loop()` |
| `Logger` | 有界日志队列、控制台/文件 sink、刷新停止 | 公共写接口线程安全 |
| `ConfigLoader` | JSON 加载、默认值、类型和范围校验 | 启动期使用，产出不可变配置 |
| `Application` | 组合根、启动顺序、优雅停止，不承担模块细节 | 主线程创建，EventLoop 运行期间协调生命周期 |

## 7. 主要类关系

```mermaid
classDiagram
    class Application
    class EventLoop
    class EpollPoller
    class Channel
    class Acceptor
    class TcpServer
    class TcpConnection
    class HttpServer
    class HttpSession
    class HttpParser
    class HttpRouter
    class TaskManager
    class TaskRepository
    class BoundedThreadPool
    class TaskExecutor
    class PluginManager
    class PluginTaskAdapter
    class IAlgorithmPlugin
    class TimerQueue
    class Logger
    class ConfigLoader

    Application *-- EventLoop
    Application *-- TcpServer
    Application *-- HttpRouter
    Application *-- TaskManager
    Application *-- PluginManager
    Application *-- Logger
    Application ..> ConfigLoader

    EventLoop *-- EpollPoller
    EventLoop *-- TimerQueue
    EventLoop o-- Channel
    TcpServer *-- Acceptor
    Acceptor *-- Channel
    TcpServer o-- TcpConnection
    TcpConnection *-- Channel
    HttpServer *-- TcpServer
    HttpServer *-- HttpRouter
    HttpServer o-- HttpSession
    HttpSession ..> TcpConnection : weak/non-owning
    HttpSession *-- HttpParser
    HttpSession --> HttpRouter

    HttpRouter --> TaskManager
    TaskManager *-- TaskRepository
    TaskManager *-- BoundedThreadPool
    TaskManager *-- TaskExecutor
    BoundedThreadPool --> TaskExecutor
    TaskManager ..> PluginTaskAdapter : validator + handler
    PluginTaskAdapter --> PluginManager
    PluginManager o-- IAlgorithmPlugin
```

所有权重点：

- `Application` 控制顶层服务的启动与销毁顺序。
- `TcpServer` 的连接表拥有 `shared_ptr<TcpConnection>`；Channel 回调避免形成强引用环。
- `TcpServer` 本身使用 shared ownership，使延迟回调捕获 server weak pointer 和
  connection strong pointer；server 必须在所有连接之后、EventLoop 之前销毁。
- fd 由 `UniqueFd` 独占，Channel 永不关闭 fd；fd 生命周期必须覆盖 Channel 的完整注册周期。
- Poller 只保存非 owning Channel 指针。Channel 禁止复制/移动；注册期地址稳定，析构断言要求它已从 Poller 移除。
- `epoll_wait` 返回的 active 指针在整个批次结束前都必须有效。任何回调都不能直接销毁本批次内的 Channel；EventLoop 在分派期间拒绝 remove。应用对象通过 `queue_in_loop` 延迟，Acceptor/TcpServer 的框架清理通过内部 `DeferredCleanup` 延迟。
- Router handler 只捕获生命周期稳定的服务引用。
- 工作任务不持有连接强引用；POST 提交立即响应，后续通过任务 ID 查询。

## 8. Reactor 与网络 I/O

### 8.1 模式选择

Phase 2 的 wakeup Channel 和 Phase 3 的监听/连接 Channel 均使用 epoll ET：

- ET 减少事件重复通知，能体现正确的非阻塞 I/O 边界处理。
- 单 Reactor 降低首版生命周期和跨线程连接状态的复杂度。
- 当前 EventLoop 只做 accept/read/write 和原始字节回调；未来业务计算必须交给工作线程。
- 首版不使用 `EPOLLONESHOT`：只有 EventLoop 线程执行连接 I/O，不存在多个 I/O worker 同时处理同一 fd；事件通常关注 `EPOLLET | EPOLLRDHUP` 加实际读写位。

### 8.2 Phase 3 accept/read/write 规则（implemented and validated）

- 使用 `socket(..., SOCK_NONBLOCK | SOCK_CLOEXEC, ...)`，不提供阻塞回退。
- 监听设置 `SO_REUSEADDR`；`SO_REUSEPORT` 首版不启用。
- `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)` 循环直到 `EAGAIN/EWOULDBLOCK`。
- 连接读取循环调用 `recv`，直到返回 `EAGAIN/EWOULDBLOCK`；`EINTR` 重试；`0` 表示 peer EOF。
- 输出使用 `send(..., MSG_NOSIGNAL)`；不依赖全局忽略 SIGPIPE。
- 写缓冲未清空时关注 `EPOLLOUT`；每次可写事件循环发送到 `EAGAIN`，清空后取消 `EPOLLOUT`，避免 busy loop。
- `EPOLLERR` 路径读取 `SO_ERROR` 并决定关闭；`EPOLLRDHUP` 或 `EPOLLHUP` 同时可读时先循环读取剩余数据。只有 HUP 且没有 read-side 事件时才直接进入关闭流程。
- close notification 以布尔门闩保证最多一次；TcpServer 使用与普通
  `queue_in_loop` 分离的 intrusive cleanup lane 延迟执行 Channel remove 和连接表
  erase，普通队列满不影响该路径。
- accepted socket 当前固定启用 `TCP_NODELAY`；配置覆盖尚未接入 AppConfig。

### 8.3 Phase 3 容量与 high-water

- 每连接只限制原始 input/output Buffer；header、body 和请求数属于未来 HTTP。
- high-water 是一次跨阈值通知，不会自动暂停读事件；降到阈值以下后允许再次通知。
- `max_output_buffer_bytes` 是独立硬上限。`send()` 在任何本次 payload 的系统发送
  或 Buffer 追加前验证 `length <= max - current_readable` 并完成整包预留；超限
  返回 `ResourceExhausted`、本次不发生部分接受，并 fail-closed。没有 timerfd，
  故尚无 idle/write deadline。
- `EPOLLOUT` 只在输出缓冲非空时启用，写空后立即停用；一次可写回调循环写到 `EAGAIN`。
- input 超限停止累计并关闭；TCP 字节层不生成协议错误响应。
- 达到最大连接数时完成 accept 后立即关闭新 fd，并记录限流事件；不能让监听 fd 在 ET 下停止 drain。

### 8.4 TcpConnection 状态与关闭

```mermaid
stateDiagram-v2
    [*] --> Connecting
    Connecting --> Connected: connect_established
    Connected --> Disconnecting: peer EOF / shutdown / close-after-write / force_close / error
    Disconnecting --> Disconnected: deferred connect_destroyed
    Connecting --> Disconnected: establishment cleanup
```

本地主动 `shutdown()` 停止新 send，先排空 output，再执行 write-half shutdown，
并等待 peer EOF；Phase 3 没有超时，非协作 peer 可使 graceful shutdown 长期停留在
Disconnecting。因此服务停止采用明确的 force-close 策略。peer EOF 路径先交付本轮
已读数据；若 message callback 产生输出，则排空后关闭。重复关闭不重复调用 close
callback。TCP 不保留消息边界；message callback 负责 retrieve。EOF 后只交付本轮新增
字节一次，部分或完全未消费的数据不会触发重复回调，并在连接销毁时丢弃。

Phase 4 为 HTTP close 新增 `close_after_write()`：同样停止新 send，但排空 output
后主动进入完整关闭与延迟销毁，不执行 write-half shutdown，也不等待 peer EOF。
该操作 owner-thread-only、幂等，close callback 仍恰好一次。它不改变
`shutdown()` 的半关闭契约。

### 8.5 内部清理、停止和回调边界

`EventLoop::queue_in_loop()` 是有界的应用/跨线程短回调入口，满时必须继续返回
`ResourceExhausted`。`DeferredCleanup` 是 owner-thread-only 的框架内部通道：节点
内嵌于 Acceptor/TcpServer，不在调度时分配内存，一个节点最多 pending 一次，链表
长度受已存在生命周期对象约束。EventLoop 在每个 active batch 后、普通 pending
callback 前后执行该链。节点 pending 时析构对象会 `std::terminate`，以运行时契约
阻止悬空 context；TcpServer 的用户可见延迟 lambda 只捕获 `weak_ptr`。

`TcpServer::stop()` 仅 owner 线程可调用，停止 accept、force-close 当前连接、幂等且
永久禁止 restart。非 active 调用同步完成，成功返回时连接表为空且 `stopped()==true`；
active callback 内调用是异步接受语义，返回时可能仍为 Stopping，批次后的内部清理
完成 Channel remove、Socket close、table erase 和断连通知后才变为 Stopped。started
server 若未完成 stop 就析构是致命契约错误。stop 期间不再调用 message/high-water，
但每个已建立连接仍可收到一次 Disconnected connection callback。

| 回调 | 抛异常后的连接策略 | EventLoop/后续连接 | 是否阻止内部清理 |
|---|---|---|---|
| connection（Connected） | 关闭该连接 | 继续 | 否 |
| connection（Disconnected） | 已处于销毁路径 | 继续 | 否 |
| message | 关闭该连接 | 继续其他连接 | 否 |
| high-water | 仅记录，连接继续 | 继续 | 否 |
| Acceptor new-connection | 本次 accepted Socket 由 RAII 关闭 | accept 循环继续 | 否 |
| close | 框架内部 hook；server 安装 `noexcept` weak 回调 | 非 server 误用异常会请求 loop stop | server 路径不会抛 |
| write-complete | Phase 3 不存在 | 不适用 | 不适用 |

所有记录操作通过 `safe_log` 隔离 Logger 异常。Acceptor 在 read callback 内 stop 时先
进入 Stopping，停止继续 accept，再通过内嵌 cleanup 节点移除监听 Channel，最后关闭
监听 Socket。

### 8.6 EventLoop 唤醒

`EventLoop::queue_in_loop()` 把短小回调放入有界队列，并写 `eventfd` 唤醒 epoll。状态检查、容量检查、入队、非阻塞写入和失败回滚由同一个 mutex 串行化：返回 failure 时本次回调不留在队列，返回 success 表示框架已经接受；显式 stop 仍可按下述状态语义取消或收尾。写入遇到 `EINTR` 重试，遇到 `EAGAIN` 视为已有待处理唤醒；其他错误回滚刚入队的回调。读取使用 `uint64_t` 并循环到 `EAGAIN`，短读、EOF 和其他错误会记录。回调先交换到局部队列，执行时不持有队列 mutex。未来工作线程不得直接调用 `epoll_ctl`、修改 Channel 或写 Socket。

Channel 事件分派语义：

1. `EPOLLHUP` 且没有 `EPOLLIN/EPOLLPRI/EPOLLRDHUP` 时调用 close callback；
2. `EPOLLERR` 调用 error callback；
3. `EPOLLIN/EPOLLPRI/EPOLLRDHUP` 任一存在时调用 read callback；
4. `EPOLLOUT` 调用 write callback。

因此 `EPOLLRDHUP` 不直接关闭，`EPOLLHUP|EPOLLIN` 仍先交给连接层读取剩余数据。
Channel 只进行通知，不读取或写出 fd；Phase 3 ET 连接层负责循环 I/O 到 `EAGAIN`。

Phase 2 状态机为：

```mermaid
stateDiagram-v2
    [*] --> Created
    Created --> Running: owner-thread run
    Created --> Stopped: stop before run, cancel queued callbacks
    Running --> Stopping: thread-safe stop
    Stopping --> Stopped: wake, drain, finish callbacks
    Stopped --> Stopped: run is rejected
```

状态与提交矩阵：

| 状态 | `run` | `queue_in_loop` | `stop` |
|---|---|---|---|
| Created | owner 可进入 Running | 接受；等待首次 run | 直接进入 Stopped，取消尚未执行回调 |
| Running | 重入拒绝 | 接受 | 原子进入 Stopping 并唤醒 epoll |
| Stopping | 拒绝 | 拒绝 | 幂等 |
| Stopped | 拒绝再次运行 | 拒绝 | 幂等 |

EventLoop 只能在构造线程析构；Running/Stopping 状态析构是违反生命周期契约的致命程序错误。正常 run 返回前一定进入 Stopped。

事件流：

```mermaid
sequenceDiagram
    participant P as Producer thread
    participant Q as Bounded callback queue
    participant E as eventfd
    participant L as EventLoop owner thread
    participant X as EpollPoller

    P->>Q: queue_in_loop(callback)
    P->>E: write uint64
    E-->>X: EPOLLIN | EPOLLET
    X-->>L: wakeup Channel
    L->>E: read until EAGAIN
    L->>Q: swap to local queue
    L->>L: execute callbacks without queue lock
```

进程在启动时屏蔽 SIGINT/SIGTERM，并计划通过 `signalfd` 把停止通知纳入 EventLoop；SIGPIPE 独立忽略。顺序必须是：主线程先屏蔽目标信号，再创建 `signalfd`，最后才创建 worker 和日志线程，避免目标信号被其他线程异步接收。信号路径只请求停止，不执行非异步信号安全的清理逻辑。

worker 完成任务后不得操作 Socket、Channel 或 epoll，也不得持有能直接操作连接的裸指针。需要通知网络侧时，worker 先写入跨线程完成队列，再写 `eventfd`；EventLoop 醒来后读取完成项，并由 EventLoop 线程更新连接状态或生成网络响应。

## 9. HTTP 解析与会话（Phase 4 implemented）

状态机：

```mermaid
stateDiagram-v2
    [*] --> RequestLine
    RequestLine --> Headers: valid CRLF line
    Headers --> Body: Content-Length > 0
    Headers --> Complete: empty line and no body
    Body --> Complete: exact body received
    RequestLine --> Error: invalid or over limit
    Headers --> Error: invalid or over limit
    Body --> Error: invalid or over limit
    Complete --> RequestLine: keep-alive and buffered next request
```

解析器按字节增量工作，不依赖 NUL 结尾，也不在不完整数据上保存悬空指针。每次返回本次消费字节数，HttpSession 立即从 TCP Buffer retrieve；body 分段复制到 Parser 自有有界字符串。限制在解析过程中立即检查：

- 请求行长度、单 header 行、header 总字节数和 header 数量；行限制包含 CRLF，
  header total 包含所有行结尾和终止空行。
- body 最大字节数。
- 所有规范化 header name 必须唯一；`Content-Length` 必须是唯一、十进制、非负、
  无溢出的值。
- 同时出现 `Transfer-Encoding` 和 `Content-Length` 时拒绝。
- chunked、trailers、Upgrade、Expect、multipart 和所有 transfer coding 不支持。
- HTTP/1.1 要求恰好一个非空 Host；method 只做 token 校验，是否允许由精确路由决定。
- 只接受 origin-form；不做 percent decode 或路径规范化，路径只用于精确路由，不提供通用文件读取。

`Connection` 按完整、大小写不敏感 token 解析；相似子串不等于 `close`，空 token
和非法 token 被拒绝。HTTP keep-alive 遵循 HTTP/1.1 默认持久连接和
`Connection: close`。顺序 pipeline
每轮最多处理 `max_requests_per_dispatch`；剩余数据通过普通有界
`queue_in_loop` continuation 继续，入队失败即 fail-closed。请求声明 close 或任一
协议/内部错误后不再处理后续 pipeline。任意组合错误的映射由固定检查顺序决定，
不依赖无序容器迭代。

响应序列化在生成输出前预检 body、header count、单行和整个 head。响应计数包含
自动生成的 `Content-Length` 与 `Connection`；字节限制包含状态行、每行 CRLF 和
终止空行。失败不返回部分响应。HTTP `Connection: close` 调用
`TcpConnection::close_after_write()`：拒绝后续 send，写尽当前输出后主动全关闭，
不等待 peer EOF。原有 `shutdown()` 仍保留为写半关闭并等待 peer EOF 的独立契约。
请求与响应共享 Header 容量限制；若配置的响应单行或总 Header 上限小到无法容纳
框架标准错误响应，序列化预检失败并关闭连接，不降级为无 `Content-Length` 的响应，
也不发送部分 Header。测试 fixture 至少为固定错误 `Content-Type` 行保留 41 字节。

### 9.1 所有权和线程边界

```mermaid
flowchart LR
    HS["HttpServer shared owner"] --> TS["TcpServer"]
    HS --> R["frozen HttpRouter"]
    HS --> M["connection-id → HttpSession"]
    TS --> C["shared TcpConnection"]
    M --> S["shared HttpSession"]
    S -. "weak, non-owning" .-> C
    S --> P["per-connection HttpParser"]
    HS -. "non-owning" .-> E["EventLoop / ILogger"]
```

- TcpServer 继续拥有 TcpConnection；HTTP 层不改变 Channel/Socket/连接表销毁顺序。
  `close_after_write()` 最终调用同一 close callback，连接表和 Session 表移除沿用
  active batch 结束后执行的内部 `DeferredCleanup`，不受普通 pending queue 容量影响。
- HttpSession 不拥有连接，也不注册 Channel；其 continuation 同时捕获 session 和
  connection weak pointer，同时至多存在一个；只有二者仍存活、Session 非 terminal
  且连接仍为 Connected 时才使用连接内部稳定 Buffer 地址继续 dispatch。
- HttpServer callback 只捕获 weak server，不捕获裸 `this`；断连回调先标记 Session
  terminal 再从表删除。
- start/stop、解析、路由和 send 都是 owner-thread-only。Router handler 必须快速，
  不得阻塞磁盘、网络或执行 AI；未来 worker 仍不得操作 HTTP/TCP/epoll 对象。
- HttpServer stop 镜像 TcpServer：禁止 restart，active batch 内可能异步完成，
  `stopped()` 同时要求底层 stopped 且 Session 表为空；未完成 stop 前析构是契约错误。
- TaskManager 声明顺序为 Validator → Repository → Executor → Pool，析构体先调用 shutdown；
  成员逆序销毁时 Pool 先析构/join，之后才销毁 Executor 和 Repository。worker closure
  捕获稳定 Executor 地址而不是裸 TaskManager；Logger 由调用者持有且必须比 Manager
  存活更久。禁止从 worker 内 shutdown 或销毁 TaskManager。

## 10. 请求数据流

### 10.1 提交任务（Phase 7 已实现的可组合 adapter）

`TaskHttpApi` 已把 HttpRouter 与 TaskManager/PluginManager 接通；CLI 仍不自动构造
该路径。`PluginTaskAdapter` 为 TaskManager 提供 validator/handler。

```mermaid
sequenceDiagram
    participant C as Client
    participant L as EventLoop
    participant H as HttpSession/Router
    participant M as TaskManager
    participant Q as ThreadPool
    participant P as PluginManager/IPlugin
    participant R as TaskRepository

    C->>L: POST /v1/tasks
    L->>H: readable bytes
    H->>H: incremental parse + JSON validation
    H->>M: submit(plugin, task_type, input, timeout)
    M->>P: validator: lookup + validate input
    M->>R: insert Queued task
    M->>Q: bounded submit
    alt accepted
        M-->>H: task_id + status_url
        H-->>C: HTTP 202
        Q->>R: Queued -> Running
        Q->>P: revalidate + execute immutable TaskRequest
        P-->>Q: PluginResult or exception
        Q->>R: Running -> Succeeded/Failed
    else queue full or shutting down
        M->>R: remove unaccepted task
        M-->>H: capacity error
        H-->>C: HTTP 503
    end
```

### 10.2 查询状态/结果

查询只读取 `TaskRepository` 快照，不等待插件完成：

- 状态接口返回当前状态、进度和时间元数据。
- 结果接口对非终态返回 409；成功返回结果；失败/超时返回结构化错误。
- 返回对象是拷贝/不可变快照，序列化期间不持有仓储锁。

### 10.3 健康检查

`GET /health` 只做常数时间状态读取，不进入线程池。后续可区分 liveness 与 readiness，但首版只提供简单健康状态。

## 11. 线程模型

```mermaid
flowchart LR
    Main["Main/bootstrap thread<br/>config, composition, signals, join"]
    Loop["EventLoop thread<br/>accept, I/O, HTTP, routing, timers"]
    W1["Worker 1"]
    W2["Worker 2"]
    WN["Worker N"]
    Repo["Thread-safe TaskRepository"]
    Log["Logger thread<br/>batch file/console flush"]

    Main --> Loop
    Loop -->|"bounded task queue"| W1
    Loop -->|"bounded task queue"| W2
    Loop -->|"bounded task queue"| WN
    Loop -->|"status/result query"| Repo
    W1 -->|"guarded transition"| Repo
    W2 -->|"guarded transition"| Repo
    WN -->|"guarded transition"| Repo
    Main --> Log
    Loop -. log records .-> Log
    W1 -. log records .-> Log
    W2 -. log records .-> Log
```

首版可以让主线程直接运行 EventLoop；逻辑上仍区分 bootstrap 阶段和事件循环阶段。并发规则：

- 连接、Channel、Buffer 和 HTTP 会话只在 EventLoop 线程访问。
- Phase 3 只有 `EventLoop::queue_in_loop()` 与 `EventLoop::stop()` 是跨线程入口；
  Acceptor start/stop、TcpServer start/stop、TcpConnection send/shutdown/close-after-write/
  force-close、
  callback setter 和连接表操作均不保证线程安全，必须由 owner 执行。
- `TaskRepository`、`BoundedThreadPool::try_submit/shutdown` 和
  `TaskManager` 公共接口内部同步；注入 validator/handler 必须允许并发调用。
- TaskManager 的 admission mutex 只保护 accepting/in-flight 计数，不跨越
  Repository 事务、Pool submit 或 handler；shutdown 关闭 admission 后通过
  condition variable 等待已获准 submit 结束。
- PluginManager 配置阶段显式注册并 freeze；本阶段没有 initialize/shutdown 生命周期、
  动态卸载或热更新。
- Frozen registry 的 lookup/validate/execute 支持并发；Manager 不持 registry mutex
  调用插件。同一实例的 validate/execute 可能同时运行，多个 worker 也可并发 execute；
  `const` 不等同线程安全，不满足并发安全的未来插件
  必须通过专用执行策略或进程隔离限制。
- 停止顺序：停止 accept → 处理/关闭连接 → 拒绝新任务 → 按策略 drain 工作队列 → shutdown 插件 → flush/stop Logger → 退出。

## 12. 任务状态与超时

合法状态转换：

```mermaid
stateDiagram-v2
    [*] --> Queued
    Queued --> Running
    Running --> Succeeded
    Running --> Failed
    Running --> TimedOut
    Succeeded --> [*]
    Failed --> [*]
    TimedOut --> [*]
```

`TaskRepository` 是任务状态转换的唯一裁决者。`Running -> Succeeded`、
`Running -> Failed` 和 `Running -> TimedOut` 的竞争在同一仓库互斥区内裁决：
第一个成功进入终态的事件获得 `Applied`，其余获得 `AlreadyTerminal`，不能覆盖
终态。TimedOut 后返回的 handler 结果被丢弃并增加 late-completion counter。
TimedOut 记录允许显式 `erase_terminal` 释放容量；删除不取消 handler，之后的
晚到 success/failure 会得到 NotFound，同样只增加 late-completion counter。TaskId
永不复用。

任务若未能进入有界队列，则不被 API 接受并从仓储移除；不会为了排队失败而扩展 `Queued -> Failed`。worker 取到任务后先转换为 Running，后续内部或插件错误才转换为 Failed。

工作队列满和 Repository 满在当前层都返回 `ResourceExhausted`。HTTP 状态映射尚未
实现；未来可以映射为 `503 Service Unavailable`，但 Phase 5 不返回 HTTP 响应。

C++17 无法安全强杀执行任意插件代码的线程。未来定时器阶段的超时语义是：

1. 截止时间到达后把仍为 Running 的任务原子地标记为 TimedOut；
2. handler 若继续运行，晚到结果被丢弃；
3. worker 线程只能在 handler 返回后复用。

Phase 5 只提供外部 `mark_timed_out` 接入点，没有 timerfd、自动扫描或
CancellationToken。因此，卡死的非协作 handler 会占用 worker 并延迟 shutdown；
真实 GPU/机器人插件必须设计可取消调用或进程隔离。

执行超时从任务进入 Running 开始。排队等待上限是独立概念；有界队列只限制容量，不能天然保证排队时延。未来可单独增加 queue-wait timeout，但不属于 Phase 1。

## 13. 定时器

选择 `timerfd + 最小堆`：

- `timerfd` 是普通 fd，可直接加入 epoll，无异步信号处理复杂度。
- 最小堆给出 `O(log n)` 添加/更新和接近 `O(1)` 的下一截止时间读取。
- 使用单调时钟避免系统时间调整影响超时。

`TimerId` 由序号和 generation 组成。取消和更新通过 generation 失效旧堆节点，过期弹出时惰性清理。回调在 EventLoop 执行，只允许关闭连接、状态转换或投递工作等短操作，禁止执行插件。

应用：

- 每次有效网络活动更新连接 idle timer。
- 任务进入 Running 时按执行超时设置 deadline timer；终态时取消。排队时间不计入
  首版执行超时。若未来增加 Cancelled，必须先扩展并测试状态机；Phase 5 没有该状态。
- 未来心跳仍复用同一 TimerQueue。

## 14. 配置模型

Phase 1 已由 `load_app_config` 把 service/runtime/logging 的最小 JSON 解析为强类型 `AppConfig`，并实现严格未知字段、类型和范围校验。后续 `ConfigLoader` 目标会在此基础上扩展以下运行时配置；类型错误、范围错误、关键资源错误必须使启动失败并返回非零退出码。

建议配置组：

- `server`：host、port、backlog、epoll_max_events、max_connections、idle_timeout_ms、TCP_NODELAY。
- `http`：request_line/header/body/input/output 上限、header_count、keep_alive_requests。
- `thread_pool`：worker_count、max_queue_size、shutdown_drain_timeout_ms。
- `tasks`：max_records、default_timeout_ms、max_timeout_ms、terminal_retention_ms。
- `logging`：level、console、file、queue_size、batch_size、flush_interval_ms、rotation_size。
- `plugins`：各插件 enabled 与专属配置。

未知顶层键默认报错，插件专属 JSON 由插件验证。日志路径必须是服务端配置，客户端不能控制。

## 15. 日志模型

Phase 1 已提供可测试的同步 `ConsoleLogger` 基础实现；Phase 8 再替换/扩展为一个有界队列和单后台写线程：

- producer 创建包含时间戳、level、线程 ID、request/task ID、可选源文件/行号的 `LogRecord`，通过 `try_push` 非阻塞入队；
- 后台线程用 condition variable 等待，按 batch 写控制台/文件并按间隔 flush，不使用 busy waiting；
- 队列预留一小段 WARN/ERROR 容量：接近满时先拒绝 TRACE/DEBUG/INFO；完全满时高等级日志也不能无限阻塞，只增加分级 dropped counter；
- 队列恢复后由后台线程输出合成的 dropped 统计，使丢弃可观测；
- 文件轮转只在后台线程按大小执行，首版不做复杂双缓冲；
- stop 后拒绝新记录、drain 队列、flush sink、join 后台线程；
- Logger 是注入实例，不是隐式单例；sink 失败不得递归写日志。

这保证日志容量固定，常规日志路径不在 EventLoop 做文件 I/O。若所有 sink 失效，进程继续还是停止由严重程度和阶段策略明确记录，不能静默忽略。

## 16. 错误处理模型

### 16.1 内部表示

预期失败使用统一值类型：

- `ErrorCode`：稳定的机器可读枚举/字符串。
- `message`：面向调用者的安全说明。
- `context`：内部诊断字段，不直接暴露给客户端。
- `std::error_code`：可选的系统错误，创建时立即捕获 `errno`。
- `request_id` / `task_id`：关联字段。

跨模块预期失败返回 `Result<T>`（C++17 自主实现的轻量 value-or-error 类型）或明确状态，不用异常控制普通流程。构造失败可抛异常，但必须在 `Application` 启动边界处理。

Phase 1 的 `Error` 保留公开字段以维持轻量值语义，因此调用者可以在构造后修改或清空 `message`，非空不能被描述为类型级永久不变量。生产路径统一使用 `make_error`；构造器、工厂和 `Result::failure` 边界会将空消息归一化为 `unspecified error`。

### 16.2 异常边界

- `main/Application`：捕获启动异常，记录后返回非零。
- EventLoop 回调：捕获到未处理异常时记录连接上下文并关闭相关连接；EventLoop 不能退出。
- worker：每个任务外围 `try/catch`，`std::exception` 和未知异常均转换为 Failed；线程继续工作。
- Phase 5 TaskExecutor：handler 异常转为 `InternalError` 和泛化消息，不把异常原文写入 Snapshot。
- Phase 6 PluginManager：metadata/validation/execution 的标准和未知异常均在插件
  边界捕获；validation 固定为 `plugin validation failed`，execution 固定为
  `plugin execution failed`，不泄露 `what()`；防御性二次验证变化固定为内部契约错误，
  输出超限在 Repository 写入前失败。
- Logger：sink 失败进入可观测降级状态，避免递归记录。

禁止空 catch 和吞错。客户端只获得稳定错误码；详细路径、errno、栈信息只进入服务端日志。

### 16.3 HTTP 映射

| 情况 | HTTP |
|---|---:|
| 非法请求行/header/JSON/字段 | 400 |
| 未认证（未来能力） | 401 |
| 未知路由、任务或插件 | 404 |
| 方法不允许 | 405 |
| 非终态查询结果、非法状态冲突 | 409 |
| body 超限 | 413 |
| 语义校验失败 | 422 |
| 连接/请求限流 | 429 或直接拒绝连接 |
| 队列满、插件未就绪、服务停止中 | 503 |
| 任务已超时 | 504 |
| 未分类内部错误 | 500 |

## 17. 安全与健壮性

- 不实现通用文件读取、下载、静态文件或 shell 执行。
- 核心只把 `input` JSON 作为数据传给已注册插件；不解释为命令。
- MockVision 只接受 `image_id` 和数值尺寸/阈值，不接受路径字段，不实际打开文件。
- 所有容量有上限；任务终态按保留期清理。
- Task ID 和 request ID 由服务端生成，客户端不能覆盖。
- 路由路径严格匹配，拒绝目录穿越形式。
- JSON 错误、插件错误和日志错误不能终止 EventLoop。
- fd 创建即设置 `CLOEXEC`，所有提前返回路径由 RAII 回收。
- 优雅停止有总超时，超时后仍不得强杀线程；应记录并由运维决定进程级处置。

## 18. 可演进点

- 多 Reactor：保留 EventLoop/Channel 的线程归属接口，但在有测量依据前不实现。
- 动态插件：未来使用版本化 C ABI 工厂而非直接假设 C++ ABI 稳定。
- 真实视觉：增加模型生命周期、GPU 上下文池、输入对象存储和独立并发闸门。
- 持久任务：以 `ITaskRepository` 抽象替换内存实现，但首版不引入数据库。
- 可观测性：后续增加 metrics/tracing，不在当前阶段宣称已有。

## 19. Phase 7 已实现边界

跨平台 `iaisf_task_api` 依赖 `http_core`、`task` 与 `plugin`，负责严格 JSON/
Content-Type 校验、`POST /v1/tasks`、末段参数形式的 `GET /v1/tasks/{id}`、
稳定 JSON 错误和安全 `TaskSnapshot` 序列化。Router 仍不是通用动态路由器：只增加
一个不解码、不规范化、只匹配一个非空末段的参数形式；exact route 始终优先。

Linux-only `iaisf_service` 是显式组合根。所有权从长到短依次为
`PluginManager -> PluginTaskAdapter -> TaskManager -> TaskHttpApi -> HttpServer`，
销毁顺序相反。PluginManager 在暴露服务前完成 Echo/MockVision 静态注册和 freeze；
TaskManager worker 不接触 EventLoop。停止顺序是关闭 Task API admission、停止 HTTP
accept/session，再 drain/join TaskManager；外部 EventLoop 与 ILogger 不归 Service
所有，也不会被 Service 停止。

`ServiceOptions` 在启动线程和监听前校验 HTTP/TCP/Task/Plugin/API 跨容量关系，不做
静默 clamp。`Created -> Running -> StoppingHttp -> StoppingTasks -> Stopped` 不允许 restart；start/stop
必须在 EventLoop owner thread。CLI 仍不默认启动该服务。

Phase 7 不包含 timerfd、自动 timeout、signalfd、取消/重试/列表/长轮询、生产 CLI
常驻模式、动态插件、GPU/真实 AI、数据库、异步日志或 benchmark。

HTTP adapter 不解析 Error message。TaskManager 的兼容 `submit()` 继续返回原
Result；`submit_with_outcome()` 在同一线性化事务外附带 TaskSubmitFailure，使
queue capacity、repository capacity、admission、validation 和 internal failure
可被稳定映射。

### 19.1 Phase 7B 停止与所有权终检

原实现可能在 `HttpServer::stop()` 仅安排 active-batch 延迟清理时立刻调用阻塞式
`TaskManager::shutdown()`，使 EventLoop 无法执行清空 Channel/Session 所必需的
continuation。最终状态机为：

```text
Created/Running
  -> StoppingHttp   (close POST admission; stop HTTP)
  -> StoppingTasks  (HttpServer stopped + session/connection tables empty)
  -> Stopped        (TaskManager drained and every worker joined)
```

`StoppingHttp` 使用 Service 自身内嵌、allocation-free 且幂等的 `DeferredCleanup`
节点；TcpServer/Acceptor 的清理节点先执行，Service continuation 再检查完成屏障。
只有 HTTP/TCP 已完全停止才允许阻塞 join。Service 不创建控制线程，不自动停止外部
EventLoop。已知边界是非协作插件会在最后阶段阻塞 owner 线程和 Service stop。

所有权为 Service 强持有 PluginManager、Adapter、TaskManager、TaskHttpApi 和
HttpServer；HttpServer 持有 frozen Router，Router handler 只弱持有 TaskHttpApi；
Adapter 生成的 validator/handler 只持有 PluginManager，不持有 Adapter、TaskManager
或 Service。成员逆序销毁先移除 HttpServer/Router，再释放 API、runtime、adapter 和
registry，不存在回到 Service 的强引用边。

跨层容量验证在任何 worker/listener/Channel/route 创建前完成。输入 envelope 使用
Plugin 的紧凑 JSON 最大 bytes 与规范 operation；输出 envelope 使用最大合法 plugin
result、最大 25-byte TaskId、operation/state/error 和全部 JSON 标点；同时校验 response
status/header line/count/total、status URL/target 以及 TCP input/output hard maximum。
所有加法均检查溢出，非法组合为 `InvalidArgument`，恰好边界接受，少 1 byte 拒绝。

TaskId 的 canonical formatter/parser 位于 task value 层且由 POST status URL、GET 和
Snapshot 共用。前缀固定为 `task-`，最少 16 位十进制；为保持 Phase 5 的完整
`uint64_t` ID 空间，17–20 位值采用无额外前导零的唯一文本，解析后必须逐字节重格式化
一致。
