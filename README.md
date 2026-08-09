# IndustrialAIServiceFramework

## Phase 10D current status

Phase 10C is committed at
`32f6a269301e971d2588b034fcb7242d92b3a4e2`; its Linux CI run
`31263414316` passed Debug and Release. Phase 10D is the current local
worktree. Its two independent real-browser workflows have passed, and the
remaining change publishes WeldAgent results from an explicit IAISF allowlist
instead of copying or recursively filtering external JSON.

Phase 10A adds a bounded Artifact Web API:
`POST /api/artifacts/v1/pointclouds` accepts direct `text/plain` XYZ input and
`GET /api/artifacts/v1/files/{artifact_id}` serves only catalog-registered,
revalidated artifacts. Uploads are canonical little-endian float32 XYZ with a
12-byte point record, SHA-256 content identity, atomic file/manifest commit,
and idempotent duplicate handling. HTTP body and response limits remain in
force (the framework hard ceiling is 64 MiB); downloads are whole-body only,
with no Range, streaming, authentication, or persistence. The catalog and
Repository are process-local and are not rebuilt after restart.

Output artifacts registered by PTV2/WeldAgent use the same catalog and receive
canonical download URLs in result JSON. The two applications remain
independent. PTV2 reports `quality_assessment=not_implemented`; WeldAgent
reports `robot_execution_allowed=false`.

The local Windows/WSL HTTP and unit evidence is current for this worktree.
The final real dual-application HTTP E2E passed with independent historical
inputs: PTV2 used 2048 points and reached `201 -> 202 -> Succeeded -> 200`;
WeldAgent used 823114 points and reached `201 -> 202 -> WaitingHuman -> 200`.
The earlier `weld_65_2047.txt` attempt remains a negative diagnostic: its 2047
points were correctly rejected by the PTV2 minimum-2048 pipeline requirement.

Final Phase 10C local matrix: Windows VS2022 Debug/Release each registered 688
tests (683 passed, 5 explicitly skipped, 0 failed); WSL Ubuntu 24.04 GCC
Debug/Release each registered 921 tests (920 passed, 1 explicitly skipped,
0 failed). Artifact HTTP targeted tests were 9/9 in all four configurations.
The current Web UI target is 7/7 and the Linux Service route target is 4/4;
these focused checks are local evidence.

Phase 10D real-browser evidence is local and is not GitHub Actions evidence.
PTV2 uploaded 2048 points, reached `Succeeded`, displayed the grey input cloud
and red 205-point overlay, reported ratio `0.10009765625` and length about
`0.8822024465 mm`, and verified all three browser downloads;
`quality_assessment=not_implemented` remains explicit. WeldAgent independently
uploaded 823114 points for requested `straight`, reached `waiting_human`, and
displayed camera/mm start/end/path, RGB axes and confidence without a corner;
`robot_execution_allowed=false` remains explicit. Real E2E exposed that the
original downloadable result could contain joint/tcp/path fields. The final
public file is generated only from the validated IAISF Domain Result, is 628
bytes with SHA-256
`71f49be2ecc3dc22c7ff49cd2cd6285e9b98a0ce1deb72d58728ed59f9001bf4`,
and contains no joint, tcp, path, URL, command, log or unknown external field.
PTV2 and WeldAgent remain separate and are never automatically chained. There
is still no weld-quality algorithm, joint generation or robot control.
Ignored browser evidence, inputs, local configuration, models and outputs are
not repository content and must not be committed.

Final Phase 10D local matrix: Windows VS2022 Debug/Release each registered 692
tests (687 passed, 5 explicitly skipped, 0 failed); WSL Ubuntu 24.04
Debug/Release each registered 925 tests (924 passed, 1 explicitly skipped,
0 failed). ApplicationAdapter is 19/19, Web UI 7/7 and Artifact HTTP 9/9 in
all four configurations; Linux Service Web UI routes are 4/4 in both WSL
configurations. Version/config smoke passed and project source/test compiler
warnings were zero.

面向工业 AI 应用的 C++ 高性能任务服务框架。

> 历史记录：Phase 9 Fast Track MVP-3 曾在本地完成并提交（`b2f16cb99bc2e4c04cdf777fc6acc56575b35b16`）。本地真实 PTV2/WeldAgent HTTP E2E 已通过；GitHub Actions 仅验证框架构建与测试，不执行真实 GPU/外部项目 E2E。

## 项目定位

项目最终目标是在 Linux 上使用 C++17、POSIX Socket、epoll ET、单 Reactor、线程池和插件机制，把工业算法封装为可提交、可查询、可超时的任务服务。

框架层只负责网络、协议、路由、任务调度、插件管理、状态、日志、配置和错误处理。焊缝、点云、机器人等领域语义只能进入插件层。

当前 `iaisf_server --config <path>` 严格校验配置后退出；Linux 上
`iaisf_server --serve --config <path>` 会由 `iaisf_app` 组合 EventLoop、静态插件、
HTTP/TCP Service 和 signalfd 停止链路并进入事件循环。Windows 保留配置校验，但
对 `--serve` 返回明确的不支持错误。

## Phase 9 Application Domain 与 Repository Core

Phase 9 Fast Track MVP-3 在上述 domain/repository 基础上完成了本地运行时接入：本地点云导入、SHA-256/manifest 校验和受控 Artifact resolver；受控跨平台进程执行；独立的 PTV2 焊后分割 adapter 与 WeldAgent 焊前建系/起终点/拐点 adapter；Application Repository、Executor 和单 worker 有界队列；六条 versioned HTTP route；以及 AppConfig、RuntimeOptions、IndustrialAiService 生命周期接入。PTV2 与 WeldAgent 完全独立，不自动串联。

本地真实 HTTP E2E 已通过：PTV2 返回 202→Succeeded→200，WeldAgent 返回 202→WaitingHuman→200。PTV2 明确返回 `quality_assessment=not_implemented`；WeldAgent 明确返回 `robot_execution_allowed=false`。这些是本地证据，不是 GitHub Actions 证据。

Phase 9/MVP-3 历史边界中尚未实现持久化 Repository、通用 Artifact Store、cancel、retry、heartbeat、lease、fencing、远程 Worker Protocol、PTV2 真实焊缝质量评价、WeldAgent joint values/轨迹下发/机器人控制和两个应用自动串联；Phase 10A 已补充本地 Artifact HTTP 上传/下载。GitHub runner 仍不执行真实 GPU/外部项目 E2E。完整边界见 [Fast Track MVP-3](docs/fast_track_mvp3.md)。

- `weld_inspection/post_weld` 与 `welding_guidance/pre_weld` 是两个完全独立的应用，不自动串联。
- `iaisf_application_core` 提供 `ApplicationIdentity`、集中式 `ApplicationJobState` 转换矩阵、公共 `ArtifactRef`、强类型 `ApplicationJobId` 和不可产生非法公开状态的 `ApplicationJobSnapshot`。
- `iaisf_application_repository` 提供结构化失败、乐观版本控制和线程安全的 `InMemoryApplicationJobRepository`。成功转换在单一临界区内校验并提交，version 精确增加 1；相同 `expected_version` 的并发转换只有一次成功。
- `ApplicationJobId` 与 `ApplicationJobSnapshot` 使用 copy-preserving move；move construction/assignment 后源对象与目标对象都保持完整合法。分配型 copy/move assignment 先复制再无分配 swap，失败不改变目标对象。
- Snapshot create/transition 与 Repository create/get/transition/erase 都执行最终 ID/值不变量检查；语法无效 ID 返回 `InvalidArgument`，合法 ID 的跨 application 访问仍返回 `NotFound`。
- Repository 容量固定且非零，不自动驱逐、不做 TTL/持久化；只允许按精确版本显式删除终态元数据。`ArtifactRef` 仍只是已验证的值引用，Repository 不读取、拥有或删除 artifact 内容。
- 跨 application 查询或更新统一返回 `NotFound`，不泄露另一应用的 Job 是否存在；`weld_inspection/post_weld` 与 `welding_guidance/pre_weld` 继续完全独立。
- PTV2 当前定位仅为分割与几何输入；独立质量能力尚未实现，必须标记 `quality_assessment=not_implemented`。
- WeldAgent 当前边界禁止真实 joint values、机器人控制及 controller URL 发送。
- MVP-3/Phase 10A 之前的历史本地验证记录：Windows VS2022 Debug/Release 全量 CTest 各 674 registered、669 passed、5 explicitly skipped、0 failed；WSL Ubuntu 24.04 GCC Debug/Release 各 903 registered、902 passed、1 explicitly skipped、0 failed。该历史记录中的 Artifact HTTP 定向测试为四套配置 9/9；WSL 是本地证据，不是 GitHub Actions 证据；Phase 9B 历史矩阵保留在历史记录中。
- Phase 9/MVP-3 历史边界不包含持久化 Repository、通用 Artifact Store、Worker Protocol 或两个业务自动串联；Phase 10A 已实现 Artifact HTTP 上传/下载。PTV2/WeldAgent adapters 与本地 HTTP API 已实现。
- 设计与边界详见 [Application Layer](docs/application_layer.md)。

## Phase 8C-2 配置系统

- `AppConfig` 是不含 fd、Socket、EventLoop 或 Linux endpoint 的可移植值对象。
- JSON schema version 当前固定为 1；缺失时按 1 处理，未知字段、重复 key、类型混用、
  非正 timeout 和超过 1 MiB 的配置文件均 fail-closed。
- 新分组 schema 覆盖 Service、Reactor、TCP、HTTP、Runtime、Task、Plugin、Task API
  和 Logging；旧顶层 `service_name`、`worker_threads`、`task_queue_capacity`、
  `log_level` 仍兼容，新旧同组写法同时出现会被拒绝。
- `make_runtime_options()` 位于 service/runtime 层，通过现有 Options 工厂完成校验，
  不让 core、TCP、HTTP、Task 或 Plugin 直接解析 JSON。
- 启用连接 idle timeout 和 HTTP timeout 时，会校验 Timer 容量至少覆盖
  `max_connections × timer_layers`；Header/Body timer 互斥，只计一层。

完整字段与启动说明见 [配置系统](docs/configuration.md)。

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

## Phase 4 已完成

- Windows/Linux 可移植 `iaisf_http_core` / `iaisf::http_core`：`HttpStatus`、`HttpLimits`、`HttpRequest`、`HttpResponse`、增量 `HttpParser`、冻结式 `HttpRouter` 和内置路由
- Linux-only `iaisf_http` / `iaisf::http`：每连接 `HttpSession` 和组合 `TcpServer` 的 `HttpServer`
- HTTP/1.1 only、严格 CRLF、origin-form target、Content-Length only、二进制 body、默认 keep-alive 和 `Connection: close`
- 有限顺序 pipelining；每轮最多处理 `max_requests_per_dispatch`，通过普通有界 `queue_in_loop` 继续，入队失败即关闭该连接
- 请求行与 header line 上限包含结尾 CRLF；header total/count、request body、response head/body、route count 全部有硬上限
- 所有重复 header name（ASCII 大小写不敏感）均 fail-closed；Host 必须恰好一个；CL+TE、歧义长度和 obs-fold 拒绝；Transfer-Encoding/chunked 返回 501，Expect 返回 417
- `Connection` 只按 comma-separated 的完整、大小写不敏感 token 识别 `close`，相似子串不匹配，空 token 或非法 token 拒绝
- `HttpResponse` 自动生成 Content-Length/Connection，拒绝 framing header 覆盖和 CRLF 注入；响应 header count、单行和 head total 预检包含自动 framing 行，失败不返回部分响应
- 请求和响应共享 Header 容量限制；若配置小到无法容纳框架标准错误响应，序列化预检失败且连接 fail-closed，不发送截断或无 framing 的响应
- 精确 method+path 路由、稳定 404/405 + Allow、handler Error/异常隔离为关闭连接的 500
- `Connection: close` 使用写尽后全关闭，不等待客户端先发 EOF；`shutdown()` 保留为独立的半关闭契约
- 每个 Session 同时至多一个 continuation；弱引用和连接状态检查避免停服/断连后再次 dispatch；Session 移除沿用不受普通 pending queue 容量影响的 `DeferredCleanup`
- 显式注册 `GET /health`（只表示 HTTP/EventLoop 可响应）与 `GET /version`
- 84 项可移植 HTTP Core 与 16 项 Linux loopback/port-0 集成测试已由最终 CI 验证；原失败的 framing/limits 集成测试在 Debug/Release 均通过

不支持 HTTP/1.0、HTTP/2、absolute/authority/asterisk-form、chunked、trailers、Upgrade、Expect、percent-decoding、路径规范化、动态路由、流式 body、TLS 或 WebSocket。

## Phase 5 已完成

- 跨平台 `iaisf_task` / `iaisf::task` 与 `iaisf_task_tests`，依赖 `iaisf::core`、`Threads::Threads` 和 nlohmann/json
- `BoundedThreadPool`：固定 worker、有界 FIFO、非阻塞 `try_submit`、队列满 `ResourceExhausted`、drain-then-join shutdown
- worker 隔离标准和未知异常；任务执行时不持有队列锁；并发 shutdown 只有首个 caller 执行 join，其他 caller 等待 Stopped
- 禁止 restart、动态 resize、detached thread 和 worker self-shutdown；join 后销毁全部 worker thread 对象
- 强类型 `TaskId`，稳定格式 `task-0000000000000001`；单进程内在线程安全仓库中单调生成，rollback/erase 后不复用，`uint64_t` 耗尽后永久拒绝
- `TaskState` 仅含 Queued、Running、Succeeded、Failed、TimedOut；不提前加入取消、重试或优先级
- `TaskLimits` 对仓库、operation、JSON input/result 和 error message 采用明确字节硬上限；JSON 按序列化 UTF-8 bytes 计数
- 有界内存 `TaskRepository` 是状态转换唯一裁决者；只允许 Queued→Running 与 Running→三个终态
- first-terminal-wins；Timeout 后晚到 success/failure 返回 AlreadyTerminal，不覆盖终态
- `TaskExecutor` 通过注入式 `TaskHandler` 执行，不接触 Socket、Channel、EventLoop、TcpConnection 或 HTTP 对象
- `TaskManager::submit` 通过 in-flight submission 计数进入事务；队列拒绝或闭包分配失败时回滚 Queued，失败提交不留可查询残留
- shutdown 的线性化点是关闭 `accepting`；随后等待 in-flight submit 完整成功或回滚，再 drain/join 已接受任务
- 正常返回、返回 Error、标准/未知异常、结果超限或 JSON 序列化失败均有 Failed/Success 终态出口；异常原文不进入 Snapshot
- TimedOut 可显式删除但不会终止仍在运行的 handler；晚到完成遇到 AlreadyTerminal 或 NotFound 都计数并丢弃
- 同一 `TaskHandler` 可被多个 worker 真正并发调用，调用者必须保证线程安全；非协作 handler 会延迟 shutdown
- Repository 满时拒绝新任务，不自动驱逐终态、不做 TTL 或持久化；容量只通过显式 `erase_terminal` 释放

Phase 5 尚未把 TaskManager 暴露为 HTTP API，也未实现自动 timeout 扫描。Windows Debug/Release 均实际执行 Foundation 43、HTTP Core 84、Task Runtime 85，合计 212/212。最终 [Linux CI run 30547126540](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540) 对实现提交 `79d3d4e89feb71595dc67d820f9a5398dcc814d4` 完成真实验证：Debug/Release 均为 Foundation 43、Reactor 45、TCP 51、HTTP Core 84、HTTP Integration 16、Task Runtime 85，合计 324/324；两个配置均实际构建 `iaisf_task` 和 `iaisf_task_tests`，项目源码和测试编译 warning 为 0。

## Phase 6 静态插件系统已完成

- 跨平台 `iaisf_plugin` / `iaisf::plugin` 与 `iaisf_plugin_tests`，不依赖 net、tcp 或 http
- `PluginLimits` 对注册数量、元数据、错误、输入/输出序列化字节、JSON 深度、总节点数、单字符串以及 capabilities 设置经校验的硬上限
- 值语义 `PluginMetadata`；operation/capability 只接受规范小写 ASCII，capabilities 有数量、字节和去重约束
- 最小 `IAlgorithmPlugin` 接口只暴露 metadata、快速无 I/O 校验和可并发 execute；同一实例的 validate/execute 可同时运行，插件作者负责线程安全
- `PluginManager` 显式注册 `shared_ptr<const IAlgorithmPlugin>`，保存一次性取得的 metadata 副本；无宏注册、全局 registry、目录扫描或动态 `.so`
- 注册表状态为 Configuring → Frozen；freeze 幂等且不可逆，冻结前 list/lookup/validate/execute 全部返回 InvalidState，冻结后并发执行这些操作；调用插件前复制 handle 并释放 registry mutex
- 重复 operation、容量耗尽、非法 metadata 和 metadata 异常均不留下半注册项；列表返回按 operation 排序的独立副本
- TaskManager 保留旧 handler-only 工厂，并新增可选 `TaskValidator`；通用限制和 validator 都在 TaskId 分配前运行，shutdown 等待 in-flight validator 完成
- Task/Plugin JSON 在插件代码前做有界结构遍历，并用无整文档副本的流式计数器精确核对 nlohmann/json 紧凑序列化字节；引号、转义、键、标点和 UTF-8 均计入，超过上限时立即终止序列化；discarded、非法 UTF-8 和非有限浮点数 fail-closed
- `PluginManager::execute` 对直接 C++ 调用也执行输入与插件校验，所有插件成功输出在进入 TaskRepository 前统一校验；输出超限转为失败而非保存半结果
- `PluginTaskAdapter` 只接受 frozen manager；生成的 validator/handler 只捕获 `shared_ptr<const PluginManager>`，不保活 Adapter 或 TaskManager，不形成强引用环；handler 对已拥有的 TaskRequest 快照做防御性二次插件校验
- 未知 operation 返回结构化 `NotFound`，不创建 TaskId、Repository 记录或线程池队列项
- validation/execute 的标准或未知异常被转换为固定安全错误；内部路径、`what()` 和原始输入不进入 TaskSnapshot，后续 worker 继续
- `EchoPlugin` 接受严格 `{"payload": <任意 JSON>}`，成功时直接返回 payload 的独立副本，不附加 operation 包装
- `MockVisionPlugin` 接受 image_id/width/height/可选 threshold，始终输出确定性 `mock: true`；它不读取图片或点云、不运行模型/GPU，不代表准确率或生产能力，不应直接驱动机器人

Windows Debug/Release clean build 与完整 CTest 均为 316/316：Foundation 43、HTTP Core 84、Task Runtime 97、Plugin System 92；Release version/config smoke 均 exit 0，项目源码和测试编译 warning 为 0。

首次功能 push run `30602538268` 对 Phase 6 功能提交 `66a606bb53bf8ed80b8efd6faf7c6529b5cd22d1` 完成 Debug/Release 428/428 与 Release smoke，但两个配置各记录 3 条项目源码 warning 和 3 条项目测试 warning，因此它不是最终封板证据。

warning 修复提交 `853ccccca80cdc042b3d51eae52fe45566aa2b22` 的最终 push run `30604428624` 在 Ubuntu 24.04.4 LTS、kernel `6.17.0-1020-azure`、GCC 13.3.0、CMake 3.31.6 上完成真实验证：Debug/Release 均为 Foundation 43、Reactor 45、TCP 51、HTTP Core 84、HTTP Integration 16、Task Runtime 97、Plugin System 92，共 428/428；`iaisf_plugin`、`iaisf_plugin_tests` 和全部专项测试实际构建/执行，Release version/config smoke 成功，项目源码 warning 0、项目测试 warning 0。两个 job 及所有步骤均为 success，无被隐藏的失败。

## Phase 7B 专项审计结果

- `IndustrialAiService::stop()` 采用 `StoppingHttp -> StoppingTasks -> Stopped` 多阶段状态机。先关闭 POST admission，再等待 `HttpServer::stopped()`、Session 表和 TCP 连接表清空，最后才阻塞 drain/join `TaskManager`；active Channel 批次中通过内嵌 `DeferredCleanup` continuation 推进，不创建控制线程，也不停止外部 EventLoop。
- 当前已知边界是：HTTP/TCP 清理完成后，EventLoop owner 线程会在 `TaskManager::shutdown()` 中等待非协作插件返回。此前不会阻塞 owner 线程所需的 Channel/Session 清理。
- `POST /v1/tasks` 的 202 body 只包含 `task_id` 与 `status_url`；它只表示任务已被接受，不再虚假承诺 `queued`、开始时间或终态。
- `ServiceOptions` 在 worker、listener、Channel 和 route 创建前完成 pool、repository、Task/Plugin JSON、TaskRequest/TaskSnapshot envelope、HTTP body/header/target 与 TCP buffer 的溢出安全交叉校验；非法组合返回 `InvalidArgument`，不 clamp。
- `TaskHttpApi` router handler 只捕获 `weak_ptr`。Service 的逆成员销毁顺序是 `HttpServer -> TaskHttpApi -> TaskManager -> PluginTaskAdapter -> PluginManager`；TaskManager 和 PluginManager 都不反向持有 API/Service，不存在强引用环。
- Failed task 的 GET 返回 HTTP 200、`state:"failed"` 和固定安全 error，不含 result、input、插件原始错误或异常文本。queue full、repository full 和 shutdown 使用 typed `TaskSubmitFailure` 分别映射稳定 503，不解析 `Error.message`。
- TaskId 统一由 `TaskId::to_string/parse` 处理：固定 `task-` 前缀，通常为 16 位十进制；为覆盖完整 `uint64_t` 范围，超过 16 位时使用唯一的无前导零表示，最长 25 bytes。正负号、空白、大小写变体、溢出及非 canonical 形式均拒绝。
- `/health` 仍只表示 HTTP/EventLoop 可响应，不声称 GPU、模型、数据库或动态插件 ready。
- Windows Debug/Release 当前均为 370/370：Foundation+smoke 43、HTTP Core 90、Task Runtime 99、Plugin System 92、Task API 46；源码和测试未出现 MSVC compiler warning。Release version/config smoke 均 exit 0。

## 尚未实现

- 生产 CLI 常驻模式与外部配置驱动的服务启动
- 动态 `.so`/DLL、插件发现、热加载、热卸载或进程隔离
- timerfd、自动任务超时扫描和连接超时
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

Phase 4 (completed)
  HTTP/1.1 core / Router / Session / HttpServer / built-in health and version

Phase 5 (completed)
  bounded fixed thread pool / Task values and limits / Repository / Executor / Manager

Phase 6 (completed)
  explicit static plugins / frozen registry / validator + handler adapter / Echo / MockVision

Phase 7 (implemented; Linux validation blocked)
  bounded Task HTTP API / static plugin composition / Linux service lifecycle

Later phases (planned)
  timers / async logging

Phase 9—10 (planned)
  measured engineering baseline -> production vision-plugin boundary
```

目标架构和阶段边界见 [architecture.md](docs/architecture.md) 与 [development_plan.md](docs/development_plan.md)。
Phase 7 协议与生命周期细节见 [task_api.md](docs/task_api.md)。

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
Phase 4 实现提交为 `9b87fdb8804ee37a8cf3b87a7b9193a3130b85d3`。首次 [Linux CI run 30537924856](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30537924856) 的 Debug/Release 各有 237/238 通过；唯一失败源于集成测试把响应 Header 单行上限设为 32 字节，小于框架固定错误响应 `Content-Type` 行的 41 字节，导致预检后 fail-closed。修复提交 `0818ebf4f71366cc3cd2fe4e36e95fe667b687a5` 将 fixture 调整到精确 41 字节并增加 portable 边界回归。最终 push [Linux CI run 30539245789](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30539245789) 实际构建四个 HTTP target，Debug/Release 均为 Foundation 43、Reactor 45、TCP 51、HTTP Core 84、HTTP Integration 16，合计 239/239；Release version/config smoke 成功，项目 warning 为 0。

Phase 5 最终 push [Linux CI run 30547126540](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540)（attempt 1）在 Ubuntu 24.04.4 LTS、kernel `6.17.0-1020-azure`、GCC 13.3.0、CMake 3.31.6 上验证提交 `79d3d4e89feb71595dc67d820f9a5398dcc814d4`。Debug/Release configure、build 和 324/324 CTest 均成功；Task Runtime 85/85，Release `--version` 与示例配置 smoke 成功，项目源码和测试编译 warning 均为 0。两个 job 和所有步骤均为 success，没有 failed、cancelled、skipped、neutral、timeout 或 `continue-on-error`。

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

真实结果见 [stage_status.md](docs/stage_status.md)。Phase 6 Windows/MSVC Debug/Release 均为 316/316：Foundation 43/43、HTTP Core 84/84、Task Runtime 97/97、Plugin System 92/92。最终 Linux Debug/Release 均为 428/428，Plugin System 92/92、Task Runtime 97/97，Release smoke 成功，项目源码和测试 warning 均为 0。

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
│   ├── plugin/
│   ├── task/
│   └── version.hpp.in
├── src/
│   ├── app/
│   ├── config/
│   ├── core/
│   ├── http/
│   ├── logging/
│   ├── net/
│   │   └── tcp/
│   ├── plugin/
│   ├── task/
│   └── main.cpp
├── tests/
│   ├── http/
│   ├── net/
│   ├── plugin/
│   └── task/
├── scripts/
└── docs/
```

`net` 包含 Phase 2 Reactor 原语和 Phase 3 `tcp/` 字节传输层；`http` 包含 Phase 4 协议核心与 Linux adapter；`task` 包含 Phase 5/6 的跨平台任务运行时与提交前验证器；`plugin` 包含 Phase 6 静态插件契约、冻结式注册表、Task 适配及两个无 I/O 内置插件。仍没有定时器或异步日志空壳类。

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
- [配置系统](docs/configuration.md)
- [Linux 构建](docs/linux_build.md)
- [分阶段计划](docs/development_plan.md)
- [协议设计](docs/protocol.md)
- [插件设计](docs/plugin_design.md)
- [测试计划](docs/test_plan.md)
- [阶段状态](docs/stage_status.md)
- [上下文交接](docs/context_handoff.md)

## Phase 7E 历史审计记录（2026-08-03）

历史记录：Linux CI run 30779555703 对提交 `1cc332b9d9e02ae78ec9e43455d36ffe939f73e2` 执行成功，但当时仍有测试 warning；该历史 run 不是最终封板证据。

但构建日志仍报告项目测试 `tests/service/test_industrial_ai_service.cpp:1041:51` 的 GCC `-Wshadow`（每个 job 3 次、共 6 条记录），因此项目测试 warning 不是 0，不能宣称 Phase 7 完成。尚未执行 `ctest --repeat until-fail:50`。此前 CLI 仍只做 version/config 校验，不启动常驻服务；当前尚未实现 timerfd/signalfd、自动任务超时、动态插件、真实 AI/GPU、数据库、异步日志、benchmark 或生产 `--serve` 模式。

Active HTTP stop 的最终契约是：停止触发请求不保证收到 503；清理可能先关闭当前连接，不能截断已经开始发送的响应。普通 Stopping 阶段新到的 POST 仍映射 503。顺序为 `TcpServer cleanup → HttpServer stopped → TaskManager shutdown/join → Service Stopped`，DeferredCleanup 必须先于阻塞任务 shutdown。

## Phase 7G 最终封板记录（2026-08-03）

状态：`PHASE_7_SERVICE_INTEGRATION_COMPLETED`。

最终 [Linux CI push run 30781932731](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30781932731)（attempt 1，event `push`，conclusion `success`）验证提交 `a44b1272bf603a17724fa17c66d60ee0e18bb918`；Debug/Release checkout 均为该 SHA。环境为 `ubuntu-24.04`、Ubuntu 24.04.4 LTS、kernel 6.17.0-1020-azure、GCC 13.3.0、CMake 3.31.6。

Debug 和 Release 均 configure/build 成功，CTest 均为 `497/497`，0 failed。标签实际数量包括 HTTP 106、integration 82、linux 127、plugin 92、service 15、smoke 2、task 99、task_api 46、tcp 51、unit 464。Release version/config smoke 成功，输出版本 `0.1.0` 和 configuration validated。项目源码 warning=0，项目测试 warning=0。

`IndustrialAiServiceTest.ActiveHttpStopWaitsForDeferredCleanupBeforeJoiningTasks` 在 Debug/Release 均通过。active 请求触发 stop 不保证返回 503，连接可能在响应生成前关闭，已开始发送的 response 不得截断；普通 Stopping 阶段的新 POST 仍返回 503。顺序为 `TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`。

尚未执行 50 次重复稳定性测试（`ctest --repeat until-fail:50`）。当前尚未实现常驻 `--serve` CLI、timerfd/signalfd、自动任务超时、动态插件、真实 AI/GPU、数据库、异步日志和 benchmark；Phase 8 尚未开始。

## Phase 8G-4E Dynamic Plugin Final Hardening (local audit)

本轮本地审计状态为 `PHASE_8G_FINAL_DYNAMIC_PLUGIN_HARDENED`。完整启动链路为：

```text
AppConfig → RuntimeOptions → DynamicPluginLoader → DynamicModule
          → Stable C ABI → DynamicPluginAdapter → PluginRuntime
          → Task adapter → TaskManager / HTTP API
```

动态插件仅在启动事务中加载。PluginRuntime 在执行 lease 全部释放后才调用
shutdown/destroy 并释放 native module；任一 create、initialize、注册、ABI、容量或
Service 启动错误都会在发布 Service 前回滚。ABI 异常被转换为有界错误，不会退出 worker。
固定、无 label 的动态指标包括 `plugin_dynamic_modules_loaded`、
`plugin_dynamic_load_failures_total` 和 `plugin_dynamic_unload_failures_total`；指标不可用或
类型不匹配时不影响插件执行。`/debug/status` 仅输出有界的状态、计数、origin 和 module_id，
不输出路径、root、config、native handle、输入输出或异常文本。

真实 MODULE fixture 与确定性 fake ABI 测试覆盖正常生命周期、create/initialize/execute/
shutdown/destroy 失败及异常、事务回滚、lease 生命周期、metrics 注入失败、诊断隐私和安全路径。
Linux/Windows 的 symlink/reparse 与权限限制均为显式测试；宿主机无法执行权限限制时记录明确 skip。
不包含热加载、远程插件、插件市场、进程隔离或 sandbox。

最终本地矩阵：WSL Ubuntu 24.04 Debug 和 Release 各 `761/761`，每个配置 1 个权限能力显式
skip、0 failures；Windows VS2022 Debug 和 Release 各注册 533 项，528 passed、5 个环境相关
显式 skip、0 failures。项目 C/C++ 源码和测试 warning 均为 0。Windows 构建中出现的非致命
`pwsh.exe` lookup 信息来自本机 Visual Studio applocal helper，不是项目编译 warning。

本轮未创建新的 GitHub Actions run（工作区仍未提交），因此本地结果不冒充远程 CI 证据；现有
workflow 已构建插件、fixture、loader/adapter tests 和 Service targets。当前构建未启用 sanitizer，
因此 ASan/UBSan 未执行且未修改 workflow。未 commit、未 push。
# Phase 8G-4D Dynamic Plugin Configuration and Service Integration

当前工作区实现了启动期动态插件配置与 Service 集成，状态为
`PHASE_8G_4D_DYNAMIC_PLUGIN_CONFIGURATION_IMPLEMENTED`。配置入口是
`plugins.runtime`；模块只在启动事务中按当前平台显式选择库文件，禁止目录扫描、PATH 搜索、热加载和远程下载。

启动顺序为：创建 `PluginRuntime` → 注册内置/静态插件 → 创建 `DynamicPluginLoader` 并加载所有启用模块 →
`register_dynamic` → `freeze` → 创建 Task adapter → 启动 HTTP。任一动态模块失败都会使 Service 创建失败，
已创建的 adapter/module 由 RAII 回滚；关闭仍遵循 HTTP/TCP cleanup → TaskManager drain/join → PluginRuntime shutdown。

本轮新增真实 fixture 动态插件、配置校验和 Service 集成测试。WSL Ubuntu Debug/Release 均为 752/752，
Windows VS2022 Debug/Release 均为 524 个测试、520 passed、4 skipped（既有环境相关），项目源码和测试 warning 均为 0。
这些是本地回归结果，当前没有为本轮提交绑定 GitHub Actions run。

## Phase 9B-3A-1：Application Submission Domain Foundation

状态：`PHASE_9B_3A_1_APPLICATION_SUBMISSION_DOMAIN_FOUNDATION_COMPLETED`（本地未提交）。
本阶段只增加不可变的 application-specific submission specification，并将其完整保存到
`ApplicationJobCreateRequest`、`ApplicationJobSnapshot` 和 Repository；没有 JSON、HTTP、
Task API/route、Job ID generator、clock、dispatcher、worker、Artifact I/O、AppConfig、
RuntimeOptions 或 Service 组合。

Inspection 只表达 `segmentation`/`geometry` 输出请求；没有质量评分、pass/fail 或插件选择。
Guidance 只保存 `auto`/`requested` weld type、`straight`/`corner`/`l` 请求和必需的人审
checkpoint；不授权机器人，也不表示算法已正确分派。后续 HTTP v1 才会将输入收窄为恰好一个
点云 Artifact。

本地验证矩阵（四套配置均重新生成并编译）：

| 配置 | registered | passed | explicitly skipped | failed | Application label |
|---|---:|---:|---:|---:|---:|
| Windows VS2022 Debug | 607 | 602 | 5 | 0 | 74/74 |
| Windows VS2022 Release | 607 | 602 | 5 | 0 | 74/74 |
| WSL Ubuntu 24.04 GCC Debug | 835 | 834 | 1 | 0 | 74/74 |
| WSL Ubuntu 24.04 GCC Release | 835 | 834 | 1 | 0 | 74/74 |

项目源码和测试编译 warning 均为 0，`git diff --check` 通过。WSL 结果是本地验证，不是 GitHub Actions；本阶段没有 commit、push 或进入 9B-3A-2。

## Phase 9B-3A-2 Strict Application JSON Contract

This phase adds contract primitives only. It does not add an HTTP route,
listener, Service integration, Job ID generator, clock, Task API migration,
Repository changes, Artifact I/O, Worker Protocol, PTV2, or WeldAgent.

`iaisf_api_common` performs bounded SAX preflight before DOM construction:
duplicate keys are rejected at every object depth, malformed UTF-8, comments,
non-finite numbers and trailing bytes fail closed, and depth, node, key/string
byte and 4 KiB request limits are centralized. Errors expose stable structured
categories without echoing request text.

`iaisf_application_contract` parses two fixed version `1.0` roots. Inspection
accepts exactly one point-cloud artifact and `segmentation`/`geometry` outputs.
Guidance accepts `auto` or `requested` (`straight`, `corner`, `l`) plus the
required human checkpoint. The parser stores validated Domain values, not the
source JSON, and performs overflow-safe `size_bytes == point_count * 12`
metadata validation. The 12-byte value is the fixed XYZ binary32 wire contract,
independent of host `sizeof(float)`. Guidance remains a request only; no robot,
controller, joint-value or automatic execution field is accepted.

The status projection is a bounded JSON body with Unix epoch milliseconds and
an application-specific status URL. It excludes artifacts, hashes, submission
specification, results, review data, quality fields and internal errors. The
Task API remains on its existing parser until the later migration phase.

Local Phase 9B-3A-2 verification: Windows VS2022 Debug/Release each had
`624 registered / 619 passed / 5 explicitly skipped / 0 failed`; WSL Ubuntu
24.04 GCC Debug/Release each had `852 registered / 851 passed / 1 explicitly
skipped / 0 failed`. The new targets ran 6 strict-JSON tests and 11 contract
tests. WSL is local validation, not GitHub Actions evidence. Project compiler
warnings were zero; WSL Debug also showed only GNU make clock-skew diagnostics.
The hardening pass contains status URL construction, JSON construction and
serialization inside the public `Result<std::string>` exception boundary. It
adds exact-limit, nested-duplicate, dangerous-field, non-finite-number,
invalid-UTF-8 and all-status-state coverage without changing the registered
test count; allocation-bearing public entry points do not claim `noexcept`.

## Phase 9B-3A-3A Application Job ID Generator and Clock

Status: `PHASE_9B_3A_3A_APPLICATION_JOB_ID_GENERATOR_AND_CLOCK_COMPLETED`.
The current worktree is intentionally uncommitted. The preceding stable
Phase 9B-3A-2 checkpoint is commit
`4d3284febd95907ecdf20f0b96aa2ab1f5044855`, validated by Linux CI run
`31140290934`.

This phase adds the portable `iaisf_application_api_primitives` static target
and alias `iaisf::application_api_primitives`. `OsApplicationJobIdGenerator`
uses `BCryptGenRandom` on Windows and `getrandom(..., 0)` on Linux to produce
exactly 16 bytes of entropy. Canonical IDs are exactly 35 ASCII bytes:
`wi_`/`wg_` followed by 32 lowercase hexadecimal characters. The generator
returns structured, bounded failure categories and never performs Repository
lookup or collision orchestration.

`IApplicationJobClock` and `SystemApplicationJobClock` provide one validated
`system_clock` read per call. Pre-epoch and unrepresentable Unix-millisecond
values fail closed. A deterministic fake clock exists only in tests; there is
no global clock or singleton.

Local validation (not GitHub Actions) completed in all four configurations:

| Configuration | Registered | Passed | Explicit skips | Failed |
|---|---:|---:|---:|---:|
| Windows VS2022 Debug | 642 | 637 | 5 | 0 |
| Windows VS2022 Release | 642 | 637 | 5 | 0 |
| WSL Ubuntu 24.04 Debug | 871 | 870 | 1 | 0 |
| WSL Ubuntu 24.04 Release | 871 | 870 | 1 | 0 |

The new `iaisf_application_api_primitives_tests` target executed 18/18 on
Windows and 19/19 on Linux; the additional Linux-only test exercises the
source-private `getrandom` seam. Version and example-config smoke commands
exited 0.
Compiler warning count was zero for project source and tests; WSL emitted
only GNU make clock-skew diagnostics caused by the shared Windows/WSL tree.

The generator uses CSPRNG entropy to reduce candidate collision probability;
Repository `create()` is the final authority for process-local successful Job
uniqueness, and 9B-3A-3A does not implement collision retry. Job IDs are not
authorization credentials. This phase does not implement HTTP routes, Service
composition, Worker Protocol, Artifact I/O, or PTV2/WeldAgent adapters. The
`weld_inspection/post_weld` and `welding_guidance/pre_weld` applications remain
independent.

The generator and generation results are non-movable; ordinary copy operations
preserve both source and destination invariants. Entropy reader contract violations are
`InternalFailure`, while OS failure/EOF is `EntropyUnavailable`. Clock
representability is checked with integer duration-ratio arithmetic, and all
deterministic/syscall seams remain private to tests.
## Phase 9 Fast Track MVP-1

The local artifact/result domain is implemented and committed locally in
`2d460e6b04dc79b3c49cd77c48613ece5d37ca8a`. See
[docs/fast_track_mvp1.md](docs/fast_track_mvp1.md). The scope is
limited to a standard-library Python importer, a filesystem-verifying C++
artifact resolver, application-specific result values, atomic Repository
completion, and bounded JSON projections. HTTP, Worker Protocol, Service
composition, persistence and uploads/downloads are outside MVP-1; the
independent PTV2/WeldAgent adapters are introduced only by MVP-2 below.

## Phase 9 Fast Track MVP-2（已由 MVP-3 接续）

The local MVP-2 checkpoint adds a bounded non-shell process runner, job-private
XYZ-f32le-to-TXT materialization, controlled output Artifact registration, and
two independent adapters. `weld_inspection/post_weld` uses the PTV2 adapter;
`welding_guidance/pre_weld` uses the WeldAgent adapter. Neither adapter mutates
Repository state or calls the other application. The PTV2 adapter keeps the
generic three-column materializer unchanged and creates a private `x y z 0`
bridge solely for the existing PTV2 loader. The fourth value is a compatibility
placeholder, not ground truth, not a model feature and not a quality score.
PTV2 output is parsed as segmentation/geometry data only and always reports
`quality_assessment=not_implemented`. WeldAgent output never permits robot
execution and never exposes joint values. Windows full CTest registered 657,
passed 652, with five explicit capability skips and zero failures in both Debug
and Release; WSL local full CTest registered 886, passed 885, with one explicit
permission-capability skip and zero failures. The new runtime target passed 9/9
in Windows Debug/Release and WSL Debug/Release. These are local results, not
GitHub Actions evidence for this historical local checkpoint.

The narrow `iaisf_ptv2_adapter_smoke` entrypoint ran the archived PTV2
executable through `Ptv2WeldInspectionAdapter + LocalProcessRunner` and exited
0: 2048 total points, 205 weld points, weld ratio 0.10009765625, length
0.8822024465, and three registered output Artifacts. A real WeldAgent pointcloud
smoke exited 0 and produced `final_result.json`; its adapter preserves human
review and `robot_execution_allowed=false`. No automatic chaining, Worker
Protocol, robot control, or quality assessment is implemented; HTTP integration
and Application Executor are provided by MVP-3 below.

## Phase 10B same-origin Web UI

Phase 10B adds a compiled-in, same-origin browser UI only when
`applications.enabled=true`. It serves exactly `GET /`, `GET /ui/app.css`,
`GET /ui/point-cloud-viewer.js` and `GET /ui/app.js`; there is no directory static-file server, external asset,
CDN, npm dependency, CORS or authentication. The UI uploads direct
`text/plain` XYZ/TXT/PTS bodies to the existing Artifact API, submits the
strict eight-field ArtifactRef to one of the two independent Application
APIs, polls with a bounded AbortController state machine, and renders result
fields and canonical Artifact download links using safe DOM APIs.

PTV2 remains `quality_assessment=not_implemented`; WeldAgent remains
`robot_execution_allowed=false`. The Phase 10B text-only baseline is extended
by the Phase 10C compiled viewer below; prediction remains download-only.

Phase 10B local validation: Windows VS2022 Debug and Release each registered
681 tests (676 passed, 5 explicit capability skips, 0 failed) after the final
narrow additions. WSL Ubuntu 24.04 Debug and Release each registered 914 tests
(913 passed, 1 explicit capability skip, 0 failed); the current Web UI target
is 7/7 and the Linux Service route target is 4/4. Host HTTP checks verified
all four resources and their security headers. Chrome browser smoke reached
the WSL listener and verified the page, same-origin resources and independent
guidance view; host-file access restrictions in Codex/Chrome automation are a
test-tool limitation, not a product blocker. These are local checks, not
GitHub Actions evidence, and no browser automation claim replaces the Phase
10A real backend E2E.

## Phase 10C browser 3D visualization MVP

The Phase 10C MVP adds a compiled-in `/ui/point-cloud-viewer.js` resource and
one independent viewer per business panel. It decodes the validated
`xyz-f32le` input, overlays the validated PTV2 ASCII PLY weld points, and
renders WeldAgent start/end/path geometry with bounded WebGL2 interaction.
`prediction.txt` remains download-only. Rendering is fail-closed and never
blocks the text result or Artifact downloads. Direction axes use `start` only
as a display anchor and are not claimed to be an algorithmic coordinate
origin. PTV2 quality remains `quality_assessment=not_implemented` and
WeldAgent remains `robot_execution_allowed=false`; the applications remain
independent. Phase 10C's commit gate used C++ resource/route contract tests;
Phase 10D subsequently supplied the separate local real-browser evidence
recorded at the top of this document. Streaming/LOD and advanced visualization
remain deferred.

Phase 10C local full CTest validation registered 688 tests in Windows VS2022
Debug and Release (683 passed, 5 explicit capability skips, 0 failed), and
921 tests in WSL Ubuntu Debug and Release (920 passed, 1 explicit capability
skip, 0 failed). The Web UI target is 7/7 and the Linux Service route target is
4/4 in each WSL run. These are local results, not GitHub Actions evidence.
