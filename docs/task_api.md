# Phase 7 Task HTTP API

## 状态与边界

当前状态为：

```text
PHASE_7_SERVICE_INTEGRATION_COMPLETED
```

`iaisf_task_api` 是 Windows/Linux 可构建的静态库；`iaisf_service` 只在
`IAISF_BUILD_LINUX_NETWORK=ON` 的 Linux 构建中出现。当前 CLI 仍只支持
`--help`、`--version` 和配置校验，不会默认监听端口。

不包含任务列表、DELETE/cancel、retry、priority、long polling、SSE、WebSocket、
timerfd 自动超时、动态插件、文件上传、真实模型或 GPU。

## 路由

### POST /v1/tasks

请求必须带唯一的 `Content-Type`，允许：

```text
application/json
application/json; charset=utf-8
```

媒体类型、参数名和 charset 值按 ASCII 大小写不敏感；只容许确定性的 OWS 和唯一的
UTF-8 charset 参数。HTTP parser 已在更早边界拒绝重复 header。

Body 是严格 JSON 根对象，只允许且必须包含：

```json
{
  "operation": "echo",
  "input": {
    "payload": {
      "value": 1
    }
  }
}
```

`operation` 必须是字符串并满足 PluginMetadata operation 规则；`input` 可以是任意
标准 JSON 类型，但仍受 TaskLimits、PluginLimits 和具体插件校验。HTTP owner thread
只执行快速、纯、无 I/O 的校验和非阻塞 `TaskManager::submit`；插件 execute 始终在线
程池 worker 中运行。

成功响应：

```http
HTTP/1.1 202 Accepted
Content-Type: application/json; charset=utf-8
```

```json
{
  "task_id": "task-0000000000000001",
  "status_url": "/v1/tasks/task-0000000000000001"
}
```

响应不包含 input、队列大小、worker 数量、时间戳或内部执行信息。202 只表示已接受，
不表示插件已经完成。

### GET /v1/tasks/{id}

只匹配 `/v1/tasks/` 后的一个非空末段；不做 percent decode、路径规范化、正则或通用
catch-all。TaskId 必须与现有 `TaskId::to_string()` 完全 round-trip：

```text
task-0000000000000001
```

Queued/Running：

```json
{
  "task_id": "task-0000000000000001",
  "operation": "echo",
  "state": "running"
}
```

Succeeded 只增加 `result`；Failed 只增加经过运行时安全归一化的 `error`。input、
created/start/finish time、线程、路径、异常 `what()` 和内部重试数据永不序列化。
本阶段没有自动超时；`timed_out` 只可能来自已有 C++ 显式状态接口。

## 错误映射

| HTTP | code | 条件 |
|---:|---|---|
| 400 | `invalid_request` / `invalid_task_id` | JSON/schema/规范 TaskId 错误 |
| 404 | `operation_not_found` / `task_not_found` / `route_not_found` | operation、task 或路由不存在 |
| 405 | `method_not_allowed` | 方法不允许，并保留 `Allow` |
| 413 | `payload_too_large` | Task/Plugin 输入容量超限 |
| 415 | `unsupported_media_type` | Content-Type 缺失或不支持 |
| 422 | `validation_failed` | 插件确定性输入校验失败 |
| 503 | `queue_full` / `repository_full` / `capacity_exhausted` / `service_stopping` | 有界资源拒绝或 admission 已关闭 |
| 500 | `internal_error` | 泛化内部失败，不泄露诊断文本 |

HTTP 映射不检查 `Error.message`。TaskManager 保留原有 `submit()`，并新增同一事务的
`submit_with_outcome()`，用 `TaskSubmitFailure` 结构化区分 InvalidRequest、
ValidationRejected、RepositoryCapacity、QueueCapacity、NotAccepting、
ResourceFailure 和 InternalFailure；原 Result/ErrorCode 兼容语义不变。

统一错误 envelope：

```json
{
  "error": {
    "code": "validation_failed",
    "message": "plugin input validation failed"
  }
}
```

响应先完整构造、紧凑 JSON 序列化并通过 HttpLimits 校验，再交给连接层；失败不会发送
半个 JSON 或错误 Content-Length。ServiceOptions 和 TaskHttpApi factory 都在启动前
校验 request/result envelope 能落入 HTTP/TCP 容量。

## 路由与生命周期

Router 仍以 exact route 为主，只新增一个“单个末段参数”表。exact route 优先；
参数 route 数量计入相同 `max_routes`，重复形状和 freeze 后注册被拒绝。TaskHttpApi
route closure 只捕获 weak token；TaskHttpApi 不拥有 EventLoop、TaskManager、
PluginManager 或 Router。

Linux Service 启动顺序：

1. 创建 PluginManager；
2. 静态注册 EchoPlugin/MockVisionPlugin；
3. freeze registry；
4. 创建 PluginTaskAdapter；
5. 创建 TaskManager；
6. 注册 built-in 与 Task API route 并 freeze Router；
7. 创建并 start HttpServer。

停止顺序：

1. TaskHttpApi 关闭 POST admission；
2. HttpServer/TcpServer 停止 accept 并关闭 session；
3. TaskManager 等待 in-flight submit，drain accepted work 并 join worker；
4. 按 HttpServer、TaskHttpApi、TaskManager、Adapter、PluginManager 的反向所有权顺序销毁。

Service 不停止外部 EventLoop；重复 stop 幂等，stop 后禁止 restart。非协作插件仍可能
无限延迟 drain，本阶段没有强制终止线程。

## 验证

Windows VS2022 x64：

| 配置 | CTest | Task API |
|---|---:|---:|
| Debug | 370/370 | 46/46 |
| Release | 370/370 | 46/46 |

Release smoke：

```text
IndustrialAIServiceFramework 0.1.0
configuration validated for service IndustrialAIServiceFramework
```

Linux-only `iaisf_service_tests` 当前包含 15 个 CTest 定义，参数展开后覆盖 37 个
GoogleTest case；均使用 loopback/port-0，覆盖 lifecycle、health/version、
Echo submit-poll-result、Mock 提交及稳定错误映射；这些测试尚未在本机 Linux 或新 CI
执行，因此不能标记 PASS。Phase 7 封板必须以当前实现提交对应的 Ubuntu 24.04
Debug/Release 完整 CTest 和 Release smoke 为准。

## Phase 7B 终检契约

- 202 只表示 accepted；即使插件在响应序列化前已完成，响应也不携带 `state`。
- stop 开始后先关闭 POST admission；仍在执行的 API 调用会在插件校验后、进入
  TaskManager 事务前再次检查 admission。GET 已存在任务在 HttpServer 真正停止前仍可查询。
- Failed task 查询是 HTTP 200，公开 error 固定为
  `{"code":"internal_error","message":"task execution failed"}`；插件返回错误、标准异常和
  未知异常都不泄漏原文。
- `TaskSnapshot` 必须满足状态/载荷不变量：Queued/Running 无 result/error，Succeeded
  只有 result，Failed 只有 error；不一致组合 fail-closed 为 API internal error。
- parameter route 只匹配一个非空末段，不 percent-decode、不规范化、不使用 regex；
  exact route 优先，额外 segment 为 404，method mismatch 为 405 且 `Allow` 去重排序。
- canonical TaskId 由 Task 类型唯一实现。常规 ID 为 `task-` 加 16 位十进制；完整
  `uint64_t` 范围允许自然扩展到 20 位数字，解析后必须逐字节 round-trip。
- API/Service factory 在分配 worker、listener、Channel 或 route 前验证最大合法
  request/response JSON（含转义后的紧凑序列化 bytes）、status URL、Header framing 与
  TCP capacity。配置恰好满足时接受，少 1 byte 时拒绝。

## Phase 7E 最终服务生命周期记录

最新 Linux CI run 30779555703 在 Debug/Release 均以 `497/497` CTest 通过，Task API 与 Service 集成测试实际执行，Release version/config smoke 成功。ActiveHttpStop 测试确认：触发 stop 的 active 请求不承诺 503，cleanup 可能先关闭连接；普通 Stopping 阶段新 POST 仍映射为 503。

停止顺序为 `TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`。DeferredCleanup 必须先于阻塞任务 shutdown，响应一旦开始发送不得被生命周期代码主动截断。当前 CLI 仍只执行 `--help`、`--version` 和配置校验，不启动常驻 HTTP 服务。

历史记录：该 run 当时因项目测试 `tests/service/test_industrial_ai_service.cpp:1041:51` 的 `-Wshadow` warning 未满足封板条件，后续提交已修复。尚未执行 `ctest --repeat until-fail:50`。timerfd/signalfd、自动任务超时、动态插件、真实 AI/GPU、数据库、异步日志和 benchmark 均未实现。

## Phase 7G 最终封板

最终 push run 30781932731（提交 `a44b1272bf603a17724fa17c66d60ee0e18bb918`）在 Debug/Release 均完成 `497/497` CTest，Task API 46、Service 15 实际执行，version/config smoke 和 ActiveHttpStop 均通过，项目源码与测试 warning 均为 0。当前状态：`PHASE_7_SERVICE_INTEGRATION_COMPLETED`。

Active stop 的请求响应和 DeferredCleanup 顺序契约不变：停止触发请求不保证 503，普通 Stopping 阶段新 POST 返回 503；`TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`。CLI 仍不启动常驻 HTTP 服务。尚未执行 50 次重复稳定性测试。
