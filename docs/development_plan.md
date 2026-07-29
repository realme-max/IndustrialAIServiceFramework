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

- CMake 最低版本建议 3.20，C++17，禁止编译器扩展。
- GCC 建议 10+ 或 Clang 建议 12+；最终最低版本在 Phase 1 Linux 编译后确定。
- 生产核心依赖保持轻量：pthread/Threads、nlohmann/json。
- 测试依赖 GoogleTest，只在 `BUILD_TESTING=ON` 时启用。
- 依赖解析优先 `find_package`；是否使用固定版本的 `FetchContent` 作为 fallback 在 Phase 1 根据离线构建要求确认。
- CMake targets 按模块拆分，使用 target 级 include、warning 和 link 设置；不使用全局 `include_directories`。

计划的构建开关：

| 选项 | 默认 | 说明 |
|---|---:|---|
| `BUILD_TESTING` | ON（开发） | 构建 GoogleTest/CTest |
| `IAISF_ENABLE_ASAN` | OFF | AddressSanitizer |
| `IAISF_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |
| `IAISF_WARNINGS_AS_ERRORS` | OFF | CI 可开启 |
| `IAISF_BUILD_EXAMPLES` | ON | 示例客户端 |

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

目标：得到一个最小、可构建、可测试、能输出版本信息的 Linux C++17 工程，不提前实现网络或异步模块。

### 4.1 实施顺序

1. 在可用 Linux 环境记录 OS、编译器、CMake 和 CPU 信息。
2. 明确许可证并创建 `LICENSE`。
3. 建立顶层 CMake 和模块 target：
   - `iaisf_core` 最小基础库；
   - `iaisf_server` 可执行程序；
   - `iaisf_tests` 或独立测试 targets。
4. 添加 `namespace iaisf`、版本常量、`ErrorCode`/`Error`/轻量 `Result<T>` 基础类型。
5. 添加强类型 `ServerConfig` 占位和默认配置对象；JSON 完整解析留到 Phase 7。
6. 添加同步 `ILogger/ConsoleLogger` 占位；异步日志留到 Phase 7。
7. `Application` 仅负责打印版本、加载最小启动参数和正常退出。
8. 添加 `config/server.json` 示例，但只包含已识别字段并标明尚未启用的字段。
9. 添加一个最小 GoogleTest 和 CTest 发现。
10. 添加 `scripts/build.sh`、`scripts/run_server.sh` 和 `scripts/smoke_test.sh`，启用 `set -euo pipefail`。
11. 使用 Debug 与 Release 各配置一次，执行测试和 smoke。
12. 更新全部阶段文档，记录真实命令和输出摘要。

### 4.2 Phase 1 文件边界

允许：

- CMake、基础 header/source、测试、配置样例和脚本。
- 版本输出、同步控制台日志、基础错误类型。

禁止：

- Socket、epoll、HTTP parser、工作线程池、TaskManager 和插件执行。
- 把占位对象描述为完整模块。
- 为通过 Windows 编译而抽象掉 Linux epoll 主线。

### 4.3 验收门槛

- Linux 下 CMake configure/build 成功。
- `ctest --test-dir build --output-on-failure` 成功。
- `iaisf_server --version` 或最小启动输出项目名、语义版本和 C++ 标准信息。
- smoke 脚本非交互、可重复、返回码可靠。
- 构建产物只在 build 目录。
- README 的命令已真实执行，结果写入 stage status。

建议 commit message：

```text
build: add phase 1 C++17 project skeleton
```

## 5. Phase 2：Socket、epoll 与 EventLoop

目标：实现 Linux ET 单 Reactor TCP echo 闭环。

交付：

- `UniqueFd`、`SocketOps`、`InetAddress`、`Buffer`
- `Channel`、`Poller`、`EpollPoller`、`EventLoop`
- `Acceptor`、`TcpServer`、`TcpConnection`
- `eventfd` 跨线程唤醒、幂等关闭、连接上限
- 非阻塞 accept/read/write、部分写、半关闭、SIGPIPE 防御
- TCP echo 示例和自动集成测试

实施重点：

- 先实现值对象/RAII，再实现 Poller/Channel，最后连接生命周期。
- ET accept/read/write 均循环到 `EAGAIN`。
- 连接容器拥有对象；Channel 不拥有 fd；删除顺序可测试。
- 通过小 socket buffer 强制触发部分写。

验收：

- 多客户端 echo 成功，连接关闭后 fd 数稳定。
- Socket 移动、epoll add/mod/del、EventLoop wakeup 有单测。
- ASan 下集成测试无明显内存/生命周期错误。
- 不实现 HTTP 和任务业务。

建议 commit message：

```text
feat(network): complete phase 2 single-reactor TCP core
```

## 6. Phase 3：HTTP 协议与路由

目标：把 TCP 字节安全转换为 HTTP/1.1 请求并提供健康检查。

交付：

- `HttpRequest`、`HttpResponse`、`HttpParser`、`HttpSession`
- 方法/路径 Router 和统一错误响应
- `GET /health`
- request line/header/body 限制和基础 keep-alive
- 400/404/405/413/500 响应

验收：

- `curl http://127.0.0.1:8080/health` 返回 200 JSON。
- 完整、逐字节/分段、非法 CRLF、非法 Content-Length、超限 body 测试通过。
- 不完整请求不会被误判为完整。
- chunked 明确拒绝，不误解析。

建议 commit message：

```text
feat(http): complete phase 3 parser routing and health API
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

- JSON 配置加载、默认值、严格类型/范围校验
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
| 当前无 Linux/WSL 环境 | 无法本机编译 epoll | Phase 1 前准备 Linux/WSL2/CI |
| 依赖获取策略未确认 | 离线构建可能失败 | Phase 1 决定系统包、vendoring 或固定 FetchContent |
| 许可证未确认 | 无法安全创建 LICENSE | Phase 1 前由用户选择 |
| 插件不协作取消 | 超时后仍占 worker | CancellationToken、晚到结果丢弃；未来进程隔离 |
| 单 Reactor 遇到慢序列化/大响应 | 网络线程延迟 | 严格大小限制；测量后再拆分 |
| 内存任务仓储不持久 | 重启丢任务 | 首版明确限制；未来通过仓储接口替换 |
| C++ 动态 ABI 不稳定 | `.so` 扩展兼容性 | 首版静态注册；未来版本化 C ABI |
| mock 被误认为真实算法 | 项目真实性受损 | schema 强制 `mock: true`，文档反复标注 |
