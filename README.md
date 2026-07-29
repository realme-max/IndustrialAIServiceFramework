# IndustrialAIServiceFramework

面向工业 AI 应用的 C++ 高性能任务服务框架。

> 当前状态：**Phase 0（架构设计）已完成**。目前只有设计文档和目录骨架，尚无可编译服务端；构建、运行、API 和测试能力均从 Phase 1 起逐步实现。本文不会把 planned 能力描述为已实现能力。

## 项目定位

IndustrialAIServiceFramework 面向 Linux 服务端，以 C++17、POSIX Socket、epoll、Reactor、线程池和插件机制为技术主线，把算法调用统一抽象为可提交、可查询、可超时的工业 AI 任务。

框架层只负责网络、协议、路由、任务调度、插件管理、状态、日志、配置和错误处理。焊缝、点云、机器人等领域语义只能出现在插件层。`PTV2-WeldSeg-Deployment` 和 `weld_agent` 不在当前工作区内，也不会在本阶段接入。

Agent、LLM、RAG、多 Agent 和机器人控制不属于当前阶段；未来 Agent 最多是一类可选编排插件。

## 目标架构

```text
Client (HTTP/1.1; framed TCP JSON is planned)
  -> Network: Socket / epoll ET / single Reactor
  -> Protocol: incremental HTTP parser
  -> Router and API services
  -> TaskManager and bounded task repository
  -> bounded ThreadPool
  -> PluginManager
  -> EchoPlugin / MockVisionPlugin / future industrial plugins
```

关键约束：

- 网络 I/O 和连接生命周期限定在一个 EventLoop 线程。
- 业务计算不得在网络线程执行。
- 队列、请求、连接和任务存储均设置容量上限。
- 第一版插件采用显式静态注册，不宣称支持动态加载 `.so`。
- MockVisionPlugin 只返回带 `mock: true` 标识的模拟结果。
- 首版不是多 Reactor、不是零拷贝，也不接入 TensorRT、PCL、真实点云或机器人。

完整设计见 [architecture.md](docs/architecture.md)、[plugin_design.md](docs/plugin_design.md) 和 [protocol.md](docs/protocol.md)。

## 技术栈

计划使用：

- C++17
- Linux、GCC 或 Clang
- CMake、CTest
- POSIX Socket、epoll、timerfd、eventfd
- `std::thread`、`std::mutex`、`std::condition_variable`
- nlohmann/json
- GoogleTest
- AddressSanitizer；UBSan、Valgrind 和 clang-tidy 为后续可选检查

核心网络层不会使用 Boost.Asio 或现成 Web 框架替代 epoll。

## 当前完成情况

已完成：

- 工作区、Git 边界、宿主工具和只读参考工程调查
- 分层架构、核心类、类关系和请求数据流设计
- 单 Reactor 线程模型、ET 读写策略和连接生命周期设计
- HTTP API、插件契约、错误模型和任务状态机设计
- 测试策略、Phase 1—9 开发计划和交接文档
- 基础空目录与 `.gitignore`

尚未实现：

- CMake 工程和任何 C++ 可执行程序
- Socket、epoll、HTTP、线程池、任务、插件、定时器、日志和配置代码
- 单元测试、集成测试、性能测试
- EchoPlugin 和 MockVisionPlugin
- 动态插件加载、真实工业视觉和机器人能力

进度事实以 [stage_status.md](docs/stage_status.md) 为准。

## 推荐项目结构

```text
IndustrialAIServiceFramework/
├── CMakeLists.txt                 # Phase 1
├── README.md
├── LICENSE                       # 待确认许可证后创建
├── config/
│   └── server.json               # Phase 1
├── docs/
├── include/iaisf/
│   ├── core/
│   ├── network/
│   ├── http/
│   ├── concurrency/
│   ├── task/
│   ├── plugin/
│   ├── timer/
│   ├── logging/
│   └── config/
├── src/
│   ├── core/
│   ├── network/
│   ├── http/
│   ├── concurrency/
│   ├── task/
│   ├── plugin/
│   ├── timer/
│   ├── logging/
│   ├── config/
│   └── main.cpp                  # Phase 1
├── plugins/
│   ├── echo/
│   └── mock_vision/
├── examples/
├── tests/
│   ├── unit/
│   └── integration/
└── scripts/
```

模块内公共接口和实现分离，业务插件不会放入 `src/` 核心目录。构建必须使用源码树外目录。

## 构建与启动

Phase 0 没有可构建目标。下面是 **Phase 1 计划采用、但当前尚未验证** 的 Linux 命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/iaisf_server --config config/server.json
```

当前宿主是 Windows 10，未检测到可运行的 WSL Linux 发行版。Windows 上的 MinGW GCC 5.3.0 既过旧，也无法编译 Linux `epoll`；因此 Phase 1 验收必须在原生 Linux、可用 WSL2 或 Linux CI 中进行。

## API 示例

以下均为 **Phase 3—5 planned**，当前不可调用。

```bash
curl http://127.0.0.1:8080/health
```

```bash
curl -X POST http://127.0.0.1:8080/api/v1/tasks \
  -H 'Content-Type: application/json' \
  -d '{"plugin":"echo","task_type":"echo","input":{"message":"hello"}}'
```

MockVisionPlugin 的结果必须包含显式模拟标识：

```json
{
  "mock": true,
  "detected": true,
  "weld_type": "straight",
  "start_point": [0.0, 0.0, 0.0],
  "end_point": [100.0, 0.0, 0.0],
  "confidence": 0.95
}
```

完整请求、响应、限制与错误映射见 [protocol.md](docs/protocol.md)。

## 测试

Phase 0 只完成测试设计，没有执行编译或运行测试。计划通过 GoogleTest + CTest 覆盖增量 HTTP 解析、ET I/O 边界、线程池停止、任务状态竞争、插件异常、定时器更新，以及端到端任务 API。

性能数字仅允许在 Phase 8 按真实硬件和原始命令测量后记录。当前没有 QPS、并发连接数、延迟、CPU 或内存结果。详见 [test_plan.md](docs/test_plan.md)。

## 与 TinyWebServer 的关系和差异

工作区中的 `TinyWebServer_reference` 仅被只读调查。借鉴范围限于 Linux Socket、epoll 事件循环、非阻塞 I/O、HTTP 状态机、线程池、超时和日志等通用思想，没有复制源码。

本项目重新设计：

- 从混合职责的 `WebServer/http_conn` 结构改为 `EventLoop`、`Channel`、`EpollPoller`、`Acceptor`、`TcpServer`、`TcpConnection` 和 `HttpSession` 等明确边界。
- 从静态数组、裸指针和全局/单例式状态改为 RAII、智能指针、实例依赖注入和 EventLoop 线程归属。
- 从 MySQL 登录、HTML 静态站点和 CGI 业务改为 JSON 任务 API；不引入数据库用户系统。
- 从连接对象直接承担业务处理改为 Router → TaskManager → ThreadPool → PluginManager。
- 从信号驱动的有序链表定时器改为 `timerfd + 最小堆`。
- 从面向连接对象的线程池改为执行通用闭包的有界、可停止线程池。
- 新增任务状态机、插件隔离、结构化错误、容量治理和自动化测试边界。

详细调查依据和自主改造点见 [architecture.md](docs/architecture.md)。

## 工业视觉插件定位

MockVisionPlugin 只验证服务框架如何承载工业任务，不读取真实点云，不调用 PCL/TensorRT，不声称具备算法精度。未来真实视觉插件必须在前序阶段稳定后再设计模型生命周期、GPU 并发、数据传输、取消和结果序列化。

## 文档索引

- [总体架构](docs/architecture.md)
- [分阶段计划](docs/development_plan.md)
- [协议设计](docs/protocol.md)
- [插件设计](docs/plugin_design.md)
- [测试计划](docs/test_plan.md)
- [阶段状态](docs/stage_status.md)
- [上下文交接](docs/context_handoff.md)

