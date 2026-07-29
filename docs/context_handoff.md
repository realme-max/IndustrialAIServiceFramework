# 上下文交接

## 1. 当前状态

- 项目：IndustrialAIServiceFramework
- 中文名：面向工业AI应用的C++高性能任务服务框架
- 当前阶段：Phase 0
- 状态：`PHASE_0_ARCHITECTURE_DESIGN_COMPLETED`
- 下一阶段：Phase 1，未开始
- 当前日期：2026-07-29
- 工作区：`E:\IndustrialAIServiceFramework`
- Git：根目录和 `TinyWebServer_reference` 都不是 Git 仓库
- 代码：没有 CMake 和 C++ 项目代码
- 验证：仅完成调查和文档一致性验证，没有编译/运行测试

## 2. 已完成模块

只有设计，没有实现：

- Network/Reactor 边界设计
- HTTP parser/router/protocol 设计
- ThreadPool/BoundedQueue 设计
- Task/Repository/Manager/Executor 与状态机设计
- Plugin 契约、静态注册和异常隔离设计
- TimerQueue 的 timerfd + 最小堆设计
- Logger 和 Config 目标设计
- Error/Result/HTTP 映射设计
- 测试和阶段计划

任何人继续工作时都不能把这些设计条目标为 implemented。

## 3. 关键文档

| 路径 | 内容 |
|---|---|
| `README.md` | 项目定位、当前真实状态、技术栈、目录、planned 命令、差异 |
| `docs/architecture.md` | 分层、核心类、类关系、数据流、线程、I/O、错误和安全 |
| `docs/development_plan.md` | Phase 0—9 交付与验收，Phase 1 详细顺序 |
| `docs/protocol.md` | HTTP API、JSON schema、限制、错误和 TCP 扩展 |
| `docs/plugin_design.md` | 插件接口、生命周期、并发、Echo/Mock、未来 ABI |
| `docs/test_plan.md` | 单元/集成/安全/sanitizer/性能测试矩阵 |
| `docs/stage_status.md` | 当前事实、环境、文件、验证和未完成项 |

## 4. 关键类（planned）

### Core

- `Application`：唯一组合根和启动/停止顺序协调者。
- `ErrorCode`、`Error`、`Result<T>`：C++17 统一预期错误模型。
- `HttpRouter`/API handlers：协议到服务层的薄适配。

### Network

- `UniqueFd`
- `InetAddress`
- `Buffer`
- `Channel`
- `Poller` / `EpollPoller`
- `EventLoop`
- `Acceptor`
- `TcpServer`
- `TcpConnection`

### HTTP

- `HttpRequest`
- `HttpResponse`
- `HttpParser`
- `HttpSession`

### Concurrency/task

- `BoundedQueue<T>`
- `ThreadPool`
- `CancellationToken`
- `Task`
- `TaskRepository`
- `TaskManager`
- `TaskExecutor`

### Plugin/timer/ops

- `PluginRequest`、`PluginResult`、`PluginMetadata`
- `IPlugin`、`PluginManager`
- `TimerId`、`TimerQueue`
- `ILogger`、`Logger`
- `ServerConfig`、`ConfigLoader`

类名和接口允许在实现评审时微调，但不得合并成类似参考工程的巨型服务器/连接业务类。

## 5. 关键路径（planned）

提交：

```text
TcpConnection
  -> HttpSession / HttpParser
  -> HttpRouter
  -> TaskManager
  -> TaskRepository (Queued)
  -> ThreadPool
  -> TaskExecutor
  -> PluginManager / IPlugin
  -> TaskRepository (Success/Failed/Timeout)
```

查询：

```text
HttpRouter
  -> TaskManager
  -> TaskRepository snapshot
  -> HttpResponse
  -> TcpConnection output buffer
```

网络对象永远不直接调用 MockVision 或未来 TensorRT/PCL/Robot 代码。

## 6. 重要设计决策

1. **单 Reactor**：首版一个 EventLoop，网络 I/O 全在该线程。
2. **ET 模式**：listen/connection 均使用 epoll ET，accept/read/write 到 `EAGAIN`。
3. **RAII fd**：`UniqueFd` 独占 fd；Channel 不拥有 fd。
4. **连接线程归属**：工作线程不得写 Socket 或修改 epoll。
5. **跨线程唤醒**：EventLoop 通过 `eventfd + post()` 接收短回调。
6. **有界资源**：连接、缓冲、线程池队列、日志队列、任务仓储都有上限。
7. **任务式 API**：插件 execute 便利端点也异步返回 task ID，不长期阻塞 HTTP。
8. **静态插件**：Application 显式注册 unique_ptr；无全局宏 registry，无 `.so`。
9. **插件并发**：首版 `execute` 需要并发安全；未来由执行策略限制 GPU 插件并发。
10. **超时语义**：C++17 cooperative cancellation；Timeout 后晚到结果丢弃，不能强杀线程。
11. **timerfd**：定时器用单调时钟、timerfd 和最小堆，不用 SIGALRM。
12. **HTTP 安全**：只支持 Content-Length，拒绝 chunked/TE+CL；严格大小限制。
13. **无文件服务**：框架不提供通用文件读取；MockVision 路径只校验、不打开。
14. **错误值优先**：预期失败用 `Result<T>`；异常只跨构造/第三方边界并在边界捕获。
15. **实例依赖**：Logger/PluginManager/Config 都不是隐式全局单例。
16. **真实性**：mock 输出强制 `"mock": true`；未测性能不写数字。
17. **执行超时**：首版 timeout 从 Running 开始，不计算有界队列等待；Queued 在停服时转 Cancelled。

## 7. 参考工程边界

只读参考：`TinyWebServer_reference/`

调查基线：

```text
files: 62
bytes: 59240225
aggregate sha256:
83AE7E469DEA30C860DEFD4D26CB313B7B3C87EFCD9387414741E152EE46CF27
```

可以借鉴：

- Socket/epoll/Reactor 的通用思想
- ET 读写到 EAGAIN
- 增量 HTTP 状态机
- 线程池、连接超时、异步日志的通用目的

不得复用：

- `WebServer`、`http_conn` 等类和接口
- 原目录结构和实现
- MySQL 登录、HTML/静态文件、CGI
- 裸连接数组、静态全局连接状态
- detached pthread 线程池
- SIGALRM + 有序链表定时器
- 参考 README 的性能数字

继续任何阶段前后都不要写入参考目录。

## 8. 当前环境

```text
OS: Windows 10 Home China 10.0.19045 x64
CPU: Intel Core i5-12400, 12 logical processors
RAM: ~15.75 GiB
Git: 2.53.0.windows.1
CMake/CTest: 4.1.0
g++: MinGW-w64 GCC 5.3.0 i686
WSL: executable present, no runnable distribution detected
```

未发现 Clang、Ninja、make、pkg-config、Docker、Valgrind、clang-format、clang-tidy。工作区没有 nlohmann/json 或 GoogleTest。

含义：不能在当前环境验证 Linux epoll。不要用旧 MinGW 的失败/成功结果代替 Linux 验收。

## 9. 构建和测试命令

当前没有可执行命令，因为 Phase 1 未开始。

Phase 1 计划命令（只有真实执行后才可写“通过”）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./scripts/smoke_test.sh
```

Phase 1 应在 stage status 记录 Linux 机器、编译器版本、完整命令和结果。

## 10. Phase 1 逐步计划

1. 得到用户明确授权后再开始。
2. 只读复查工作区和 Git；保护新增的用户修改。
3. 确认 Linux/WSL2/CI、许可证、依赖获取方式。
4. 创建 `CMakeLists.txt`，建立 target 化 C++17 工程和 out-of-source 规则。
5. 创建最小 `namespace iaisf`、版本、Error/Result。
6. 创建 Config 和同步 ConsoleLogger 占位；清楚标注不完整。
7. `main/Application` 只输出版本/启动信息，不实现 Socket。
8. 创建最小 GoogleTest/CTest。
9. 创建 server.json、build/run/smoke 脚本。
10. 在 Linux Debug/Release 实际构建、测试。
11. 更新 README、stage status、handoff 和测试结果。
12. 停在 Phase 1，不顺手实现 Phase 2。

## 11. Phase 1 入口待确认

- 许可证：MIT 还是 Apache-2.0（或用户指定）。
- Linux 环境：原生、WSL2 还是 CI。
- 依赖：系统包、固定 FetchContent 还是 vendored。
- Phase 1 是否初始化 Git；用户只说不得自动 commit/push，没有要求 `git init`，因此默认不自动初始化，除非明确要求。

这些问题不影响 Phase 0 设计，但会影响 Phase 1 文件和可复现构建。

## 12. 已知问题与风险

- 无 Linux 环境，尚未验证任何 API/编译器假设。
- `Result<T>` 的具体实现、错误码全集和 CMake target 划分要在 Phase 1 小步确定。
- HTTP parser 的 chunked 拒绝码需要在 Phase 3 固定为 400 或 501 并测试；当前设计允许二选一，但实现后不得漂移。
- Phase 5 必须实现快速、无 I/O 的 `validate_request`，使确定性 task_type/input 错误在排队前返回 422。
- 单 Reactor 对大 JSON 序列化的上限需要 Phase 8 测量。
- 非协作插件超时后会继续占 worker，真实插件可能需要进程隔离。
- 内存任务存储重启丢失是首版明确限制。

## 13. 不能破坏的约束

- C++17、Linux、原生 epoll 核心；不用 Boost.Asio/Web 框架替代。
- 不复制或改名 TinyWebServer。
- 框架层不出现 PointNet++、PCL、TensorRT、焊缝或机器人业务类。
- 不把 Agent 作为主线，不引入 LLM/LangGraph/MCP/RAG/多 Agent。
- 不使用裸 `new/delete`、全局可变状态、无界队列或不受控线程。
- 不允许网络线程执行耗时插件。
- 不提供 shell、任意文件、远程代码执行或目录穿越。
- 不编造性能、算法、GPU、机器人或动态插件能力。
- 不修改其他项目，不自动 commit/push/PR。
- 每阶段必须更新 README、stage status、handoff、设计和真实测试结果。

## 14. 建议提交信息

Phase 0 未 commit。若用户明确要求提交：

```text
docs: complete phase 0 architecture design
```
