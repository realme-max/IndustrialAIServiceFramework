# Configuration System

## 状态与边界

Phase 8C-2 当前状态为 `PHASE_8C_2_CONFIGURATION_SYSTEM_IMPLEMENTED`。本模块只支持
本地 JSON 文件的一次性启动加载，不支持 YAML、环境变量覆盖、热更新或全局配置单例。
网络、HTTP、任务和插件模块都不读取 JSON。

## 配置模型

```text
AppConfig
├── ServiceConfig
├── ServerConfig
│   ├── ReactorConfig
│   └── TcpConfig
├── HttpConfig
├── RuntimeConfig
├── TaskConfig
├── PluginConfig
├── TaskApiConfig
└── LoggingConfig
```

这些类型只保存 string、integer、bool、optional 和 enum。`AppConfig` 经
`make_runtime_options()` 转换为 EventLoop、TimerQueue、TcpServer、HTTP limits 和
ServiceOptions 所需的已校验对象。转换位于 service/runtime 层，因此依赖方向保持为
`core <- service <- app`，core 不依赖 service。

## JSON 规则

- `schema_version` 缺失时为 1；任何非整数 1 均拒绝。
- 每个对象只接受声明字段，未知字段拒绝。
- integer 不接受 float、string 或 bool。
- 任一 object 内重复 key 拒绝。
- 文件最大 1 MiB。
- timeout 只接受 `null` 或正整数毫秒，0 拒绝。
- 新分组 schema 与旧顶层 `service_name`、`worker_threads`、
  `task_queue_capacity`、`log_level` 兼容；同组新旧字段混用因含义歧义而拒绝。
- 错误使用 `Result<T>` 返回稳定 ErrorCode，不把本地配置路径写入错误文本。

## 映射与跨字段约束

`RuntimeOptions` 复用现有组件的 `create()`/验证入口，而不是复制其限制。映射包含：

- `server.reactor` → EventLoop 容量和 TimerQueueOptions；
- `server.tcp` → endpoint、TcpServerOptions 与 idle timeout；
- `http` → HttpLimits、header/body inactivity timeout；
- `runtime` → 固定线程池 worker 和有界队列；
- `tasks`、`plugins`、`task_api` → 对应 limits 和 ServiceOptions；
- plugin enable 开关 → Service 启动时的静态 Echo/MockVision 注册。

Timer 容量按每连接的活动层校验：启用 TCP idle timeout 计一层；任一 HTTP header/body
timeout 启用再计一层。HTTP header/body timer 在单个 Session 内互斥，因此共同启用仍只
计一层。容量不足返回 ConfigError，启动前不创建线程、fd 或 listener。

## CLI 启动流程

```text
CLI parse
  -> load_app_config
  -> make_runtime_options
  -> EventLoop::create
  -> IndustrialAiService::create/start
  -> install shutdown signals through the Service
  -> EventLoop::run
  -> Service stop/drain
```

`--config <path>` 只加载、校验并退出。Linux 的
`--serve --config <path>` 执行上述组合；SIGINT/SIGTERM 通过既有 signalfd → Service
完整停止链路，不直接绕过 Service 调用 EventLoop stop。Windows 对 `--serve` 明确返回
不支持，仍可执行 portable 配置校验。

## 验证边界

Windows VS2022 Debug/Release 均为 `380/380`，本地 WSL Ubuntu 24.04 GCC
Debug/Release 均为 `596/596`；两平台 version/config smoke 均成功，项目源码和测试
编译 warning 为 0。当前未提交改动尚无对应 GitHub Actions 运行证据，本地 WSL 结果
不得描述为远端 CI。
