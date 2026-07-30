# 分阶段开发计划

## 1. 执行原则

项目严格按 Phase 0—9 推进。每个阶段只在其验收门槛通过后进入下一阶段，并同步更新：

- `README.md`
- `docs/stage_status.md`
- `docs/context_handoff.md`
- 对应设计文档
- 实际测试命令和结果

通用约束：

- 只在 Linux 环境验收 epoll 服务；Windows/MinGW 不能代替目标环境。
- 源码树外构建，禁止提交 build、日志、临时文件和测试产物。
- 不自动 commit，不 push，不创建 PR。
- 不修改 `TinyWebServer_reference`、`PTV2-WeldSeg-Deployment` 或 `weld_agent`。
- 不把 planned、mock 或参考工程能力写成已实现能力。
- 每阶段先实现最小闭环，再增加复杂性。

## 2. 依赖与构建策略

基线：

- CMake 最低版本 3.22，C++17，禁止编译器扩展。
- GCC 建议 10+ 或 Clang 建议 12+；Phase 1 已在 GCC 13.3.0 上验证，更低版本兼容性仍需单独测试。
- Phase 1 生产核心依赖为 nlohmann/json `v3.11.3`；Phase 2 的 Linux-only `iaisf_net` 通过 `Threads::Threads` 表达线程运行时依赖。
- 测试依赖 GoogleTest `v1.15.2`，只在 `IAISF_BUILD_TESTS=ON` 时启用。
- 默认使用固定 tag 的 FetchContent；`IAISF_USE_SYSTEM_DEPS=ON` 时严格通过 `find_package` 查找，不静默 fallback。
- CMake targets 按模块拆分，使用 target 级 include、warning 和 link 设置；不使用全局 `include_directories`。

计划的构建开关：

| 选项 | 默认 | 说明 |
|---|---:|---|
| `IAISF_BUILD_TESTS` | ON | 构建 GoogleTest/CTest；作为子项目可显式关闭 |
| `IAISF_USE_SYSTEM_DEPS` | OFF | OFF 使用固定 FetchContent，ON 使用系统包 |
| `IAISF_BUILD_LINUX_NETWORK` | Linux ON；其他平台 OFF | 构建 epoll/eventfd Reactor；非 Linux 显式设为 ON 时 configure 失败 |

Sanitizer、examples 和 warnings-as-errors 开关尚未实现，按后续阶段需要增加。

## 3. Phase 0：调查与架构设计

状态：**completed（2026-07-29）**

完成内容：

- 只读调查工作区、Git、宿主工具链和 `TinyWebServer_reference`。
- 明确参考范围、自主模块边界和禁止复用项。
- 设计网络、HTTP、任务、插件、定时器、日志、配置和错误模型。
- 设计类关系、线程模型、数据流、协议和测试矩阵。
- 创建文档、`.gitignore` 和基础空目录。

验证：

- 未发现当前或嵌套 Git 仓库，因而没有覆盖未提交修改。
- 参考目录调查前后应通过文件数、总大小和聚合哈希复核。
- 没有创建 CMakeLists、C++ 源文件、配置文件或可执行程序。
- 没有编译、运行、commit 或 push。

建议 commit message（仅建议，未执行）：

```text
docs: complete phase 0 architecture design
```

## 4. Phase 1：项目骨架与构建系统

状态：**completed（2026-07-30）**

已实现：

- CMake 3.22+、C++17、target 级 warning 和生成版本头。
- `iaisf_core`、`iaisf_server`、`iaisf_tests`。
- `ErrorCode/Error`、`Result<T>`、`Result<void>`。
- nlohmann/json 严格 `AppConfig`，含默认值、范围和未知字段校验。
- `LogLevel`、`ILogger`、同步 `ConsoleLogger`。
- `Application` 的 `--help`、`--version`、`--config <path>`、无参数和非法参数行为。
- GoogleTest/CTest、真实示例配置测试和两个 CLI smoke tests。
- Linux Debug/Release build/test/smoke 脚本和 Linux 构建文档。
- Apache License 2.0。
- Phase 1B 完整代码审计和针对 Error、Result、AppConfig、CMake 的修正。
- `.github/workflows/linux-ci.yml`：Ubuntu 24.04 GCC Debug/Release、CTest 和 Release smoke。

验证：

- Phase 1 最终实现提交为 `63b30cffcbe3e621af33664721b3675a647bd1a1`。
- [GitHub Actions run 30508113122](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30508113122) 在 `phase/1-foundation` 上以 `ubuntu-24.04` runner 完成，workflow conclusion 为 `success`。
- Linux Debug configure/build 成功，CTest 43/43 通过、0 failed。
- Linux Release configure/build 成功，CTest 43/43 通过、0 failed；`--version` 和示例配置 smoke 成功。
- CI 环境记录 Ubuntu 24.04.4 LTS、GCC 13.3.0、CMake 3.31.6；项目编译日志未出现 warning。
- Windows Visual Studio 2022 x64 作为补充检查：Debug/Release configure、build 和 43 个 CTest 均成功；CLI version/config smoke 退出码均为 0。
- 最终项目自身 MSVC 编译无 C++ warning；MSBuild 环境仍打印非致命的 `pwsh.exe` 缺失诊断。
- 未实现任何 Socket、epoll、HTTP、线程池、任务、插件或异步日志能力。

Phase 1C 建议 commit message：

```text
docs: complete phase 1 validation record
```

## 5. Phase 2：Socket、epoll 与 EventLoop

状态：**completed（2026-07-30）**

已实现 Linux fd RAII、Socket 基础操作与单 Reactor 事件循环的最小基础设施。
Reactor 实现提交为 `f76993e09767a2d6b6e1cbd2bcb22cfa1df6f74f`，warning 修复及
最终验证提交为 `4db8708a5121f8477d835addd0b16170a3e2054f`；后者已由
[GitHub Actions run 30516007475](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30516007475)
完成真实 Linux Debug/Release 零 warning 验证。

已交付：

- `UniqueFd`
- Linux Socket 基础封装
- `Channel`
- `EpollPoller`
- `EventLoop`
- `eventfd` 跨线程唤醒
- 有界跨线程回调队列
- `iaisf_net` 静态库与 `iaisf::net` alias
- 44 个 Linux-only Reactor 测试定义

已固定的实现契约：

- `UniqueFd` 独占 fd、禁止复制、允许移动；析构时只尝试一次 `close`，不对 `EINTR` 重试。
- `Socket` 创建 nonblocking、close-on-exec 的 IPv4 TCP fd；本阶段不提供 bind/listen/accept/connect。
- `Channel` 不拥有 fd；稳定地址和 fd 的生命周期必须长于注册期，析构前必须移除。active 批次内禁止直接 remove/destroy，必须通过 `queue_in_loop` 延迟。
- `EPOLLRDHUP` 是 read-side 通知；`HUP|IN` 仍执行 read，只有没有 read-side 事件的 HUP 才直接 close。Channel 不负责 ET drain。
- `EventLoop` 构造线程是 owner；`run/update_channel/remove_channel` 仅 owner 可调用，`queue_in_loop/stop` 可跨线程调用。
- epoll 使用 ET，不启用 ONESHOT；`eventfd` 只承担唤醒并读取到 `EAGAIN`。
- 待执行队列按元素数限制；空回调和队列满返回明确 Error。状态/容量检查、入队、唤醒和失败回滚具有原子接受语义。
- Created 可预入队；run 前 stop 直接进入 Stopped 并取消队列。Running stop 进入 Stopping，Stopping/Stopped 拒绝新提交。
- 单个 Channel 回调异常终止该 Channel 本轮剩余分派，但后续 active Channel 继续；pending callback 异常逐个隔离。

当前验证：

- Windows VS2022 Debug/Release Phase 1 回归均为 43/43 CTest 通过；Release 两项 CLI smoke 均成功。
- 非 Linux 显式 `IAISF_BUILD_LINUX_NETWORK=ON` 的 CMake 负向检查按预期失败并给出明确诊断。
- shell 语法、workflow YAML 解析、禁止实现项扫描和 `git diff --check` 已通过。
- 首次功能 run `30514521602` 对应 Reactor 实现提交，Debug/Release 功能和测试通过，但 Release 测试构建有 2 条 `-Wunused-result` warning。
- 最终零 warning run `30516007475` 对应 warning 修复提交；Ubuntu 24.04.4 LTS、GCC 13.3.0、CMake 3.31.6 的 Debug/Release configure 和 build 均成功。
- Debug CTest 87/87、Release CTest 87/87，均为 0 failed；每个配置实际执行 44 个 Reactor 测试：UniqueFd 8、Socket 5、Channel 7、EpollPoller 7、EventLoop 17。
- Release `--version` 输出 `IndustrialAIServiceFramework 0.1.0`，示例配置输出包含 `configuration validated for service IndustrialAIServiceFramework`。
- workflow attempt 1 最终 conclusion 为 `success`；两个 job 和所有步骤均为 `success`，没有 skipped、cancelled 或 `continue-on-error`。
- 最终 Debug/Release 编译日志中项目源码 warning 为 0、项目测试 warning 为 0；两处被忽略的 `read()` 返回值已由 warning 修复提交验证消除。
- 不实现 HTTP、完整 `TcpConnection` 协议处理、ThreadPool、TaskRepository、PluginManager、timerfd 任务超时、signalfd 优雅停止、异步日志或 AI 插件。

建议 commit message：

```text
docs: complete phase 2 validation record
```

## 6. Phase 3：TCP 连接层

状态：**planned，未开始**。Phase 2 已完成 Linux CI 验收；仍需用户明确要求后才能开始本阶段。

目标：在现有单 Reactor Core 上建立完整、可测试的 TCP 连接生命周期，不引入 HTTP 或业务执行。

交付：

- `Buffer`
- `Acceptor`
- `TcpConnection`
- `TcpServer`
- 输入缓冲区和输出缓冲区
- ET accept/read/write 循环到 `EAGAIN`
- 输出缓冲非空时启用、写空后停用 `EPOLLOUT`
- 输出缓冲高水位和慢客户端边界
- 连接建立、读半关闭、写半关闭和最终关闭生命周期
- 基础回显或原始字节集成测试

验收：

- ET accept/read/write 均 drain 到 `EAGAIN`，不会因单次系统调用遗漏事件。
- 多客户端基础回显或原始字节传输自动化测试通过。
- `EPOLLOUT` 仅在有待发送数据时关注，不产生空转。
- peer 半关闭后仍能处理已接收数据并按约定完成关闭。
- Channel 移除和连接销毁遵守 active batch 延迟规则，无 fd 泄漏。

明确不包含：

- HTTP parser、HttpRouter 或 `/health`
- ThreadPool、TaskRepository、TaskManager、PluginManager
- timerfd、signalfd、异步日志
- AI 推理、工业业务插件或 benchmark

建议 commit message：

```text
feat(net): implement TCP connection layer
```

## 7. Phase 4：线程池与任务系统

目标：实现无需插件也可测试的异步任务生命周期。

交付：

- `BoundedQueue<std::function<void()>>`、`ThreadPool`
- `Task`、`TaskStatus`、`TaskRepository`、`TaskManager`
- 合法状态转换、内存容量和终态快照
- `POST /api/v1/tasks`、状态查询、结果查询的服务层与路由
- 暂用内置测试执行器，不冒充插件系统

验收：

- 任务在 worker 执行，EventLoop 不运行耗时函数。
- 队列满返回 503；停止后拒绝新任务。
- worker 捕获异常并继续执行下一任务。
- drain 与 immediate reject 语义有确定测试。
- 并发状态转换无数据竞争；ThreadSanitizer 可用时作为附加检查。

建议 commit message：

```text
feat(task): complete phase 4 bounded async task system
```

## 8. Phase 5：插件系统

目标：通过静态注册插件完成工业任务服务化闭环。

交付：

- `PluginRequest`、`PluginResult`、`IPlugin`、`PluginManager`
- 显式静态注册、快速无 I/O 的 `validate_request`、重复名/未知名/初始化失败处理
- `EchoPlugin`
- `MockVisionPlugin`，所有结果带 `mock: true`
- `POST /api/v1/plugins/{plugin_name}/execute`
- 插件异常隔离和配置传递

验收：

- HTTP 指定插件可异步提交并查询结果。
- 未知插件、错误 task_type 和插件异常返回稳定错误。
- MockVision 不读取点云、不依赖 GPU/PCL/TensorRT。
- 核心 target 不链接具体插件依赖。

建议 commit message：

```text
feat(plugin): complete phase 5 static plugin execution
```

## 9. Phase 6：定时器和任务超时

目标：实现 EventLoop 内统一超时调度。

交付：

- `TimerId`、`TimerQueue`、最小堆、`timerfd`
- add/cancel/update/next-expiry/run-expired
- 连接 idle timeout
- 任务 deadline、CancellationToken、晚到结果丢弃
- 终态任务保留期清理

验收：

- 已取消 timer 不执行，更新 timer 只执行新 generation。
- 空闲连接关闭，活动连接续期。
- 超时与成功竞争只有一个终态。
- 非协作插件的限制在文档和测试中明确。

建议 commit message：

```text
feat(timer): complete phase 6 connection and task timeouts
```

## 10. Phase 7：异步日志与配置完善

目标：用强类型配置和有界异步日志替换 Phase 1 占位。

交付：

- 扩展 Phase 1 AppConfig，加入 server/http/task/plugin/logging 运行时配置及跨字段校验
- 有界日志队列、后台线程、批量写、flush、关闭
- 控制台/文件 sink、级别、基础按大小轮转
- request_id/task_id 上下文
- 配置和日志失败的明确启动/降级策略

验收：

- 错误配置启动失败且返回非零。
- 多线程日志行完整，退出前可验证刷新。
- 队列溢出策略可观测，不形成无限内存增长。
- 网络线程日志路径不进行常规同步文件写。

建议 commit message：

```text
feat(ops): complete phase 7 validated config and async logging
```

## 11. Phase 8：压力测试与工程完善

目标：测量而不是预设性能，并完善质量门禁。

交付：

- wrk/ab 基础 HTTP 测试
- 多连接和多任务并发测试
- 延迟、吞吐、CPU、内存、稳定性记录
- ASan、UBSan；Valgrind/clang-tidy 按环境可用性执行
- clang-format 配置

每组结果必须记录：

- 日期、Git commit（若有）、操作系统、内核、CPU、内存
- 编译器与版本、Debug/Release、编译选项
- worker 数、连接限制、队列限制
- 请求类型、并发数、持续时间、原始命令
- 原始输出保存位置和结果解释

在此阶段前，README 性能章节保持“未测量”。

建议 commit message：

```text
test(perf): complete phase 8 measured performance baseline
```

## 12. Phase 9：真实工业视觉插件预留

目标：只设计可接入接口，不直接迁移 PTV2 代码。

研究项：

- 模型加载/卸载和 warmup 生命周期
- 单/多 GPU 推理上下文和并发限制
- 输入对象存储、点云上传和大小限制
- PCL/TensorRT 错误映射
- cooperative cancellation 与不可取消内核
- 结果 schema、版本和大结果传输
- 插件进程隔离的必要性

进入条件：

- Phase 1—8 验收通过并有稳定性数据。
- 用户明确批准开始。
- PTV2 项目访问边界和许可证已确认。

建议 commit message：

```text
docs(vision): define phase 9 production vision plugin boundary
```

## 13. 风险登记

| 风险 | 影响 | 当前缓解 |
|---|---|---|
| 本机无 Linux/WSL 环境 | 无法本机复现 Linux CI | Ubuntu 24.04 GitHub Actions 已完成 Phase 1 验收；保留完整 run 证据 |
| 默认 FetchContent 依赖网络 | 首次离线配置失败 | 固定 tag；提供系统依赖模式 |
| 插件不协作取消 | 超时后仍占 worker | CancellationToken、晚到结果丢弃；未来进程隔离 |
| 单 Reactor 遇到慢序列化/大响应 | 网络线程延迟 | 严格大小限制；测量后再拆分 |
| 内存任务仓储不持久 | 重启丢任务 | 首版明确限制；未来通过仓储接口替换 |
| C++ 动态 ABI 不稳定 | `.so` 扩展兼容性 | 首版静态注册；未来版本化 C ABI |
| mock 被误认为真实算法 | 项目真实性受损 | schema 强制 `mock: true`，文档反复标注 |
