# 协议设计

## 1. 范围与状态

Phase 4 HTTP 协议库状态保持 `PHASE_4_HTTP_PROTOCOL_COMPLETED`。Phase 5 总体状态为
`PHASE_5_TASK_RUNTIME_IMPLEMENTED_LINUX_VALIDATION_BLOCKED`。可移植
`iaisf_http_core` 已在 Windows Debug/Release 各通过 84/84 HTTP Core 测试；
最终 [Linux CI run 30539245789](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30539245789)
的 Debug/Release 均为 239/239，其中 HTTP Core 84/84、Linux-only
HttpSession/HttpServer integration 16/16。

当前 CLI 不启动监听；`/health`、`/version` 是显式注册到 `HttpRouter` 后由
`HttpServer` API 提供的能力。Phase 5 Task Runtime 不依赖 HTTP，后文任务 JSON API
仍是 planned，不得当作当前端点；当前不存在 `/v1/tasks` 或 `/api/v1/tasks` 路由。
服务不提供 HTML、文件下载、任意路径读取、shell 或客户端代码执行。

## 2. HTTP 基线

- 只接受 `HTTP/1.1` 和严格 `CRLF`。
- 只接受以 `/` 开头的 origin-form target；method 保留原始大小写并按 RFC token
  校验，路由比较区分大小写。
- 请求/响应 framing 只使用 `Content-Length`；body 是 binary-safe 字节串，不假设
  JSON 或 UTF-8。
- HTTP/1.1 默认 keep-alive；`Connection` 按 comma-separated、大小写不敏感 token
  解析，只有完整 token `close` 才在当前响应写尽后主动全关闭；`xclose` 和
  `close-x` 不匹配，空 token 或非法 token 拒绝。
- 支持有界、同步、顺序 pipelining，不并行也不乱序。
- 不支持 HTTP/1.0、HTTP/2、absolute/authority/asterisk-form、Transfer-Encoding、
  chunked、trailers、Upgrade、Expect、流式 body、percent decode、路径规范化、
  动态参数、HTTPS、multipart 或 WebSocket。
- `/health`、`/version` 使用 `application/json`；框架错误响应使用
  `text/plain; charset=utf-8`。所有响应自动写准确 `Content-Length` 和明确
  `Connection`。

### 2.1 Header 与请求走私防护

- header name 转为 ASCII lowercase，value 去除首尾 SP/HTAB；拒绝空名称、colon
  前空白、非法 token、NUL/CR/LF/控制字符、obs-fold、bare CR/LF。
- 所有重复的规范化 header name 均返回 400；不采用 first-wins、last-wins 或合并
  语义，且不同 ASCII 大小写仍视为重复。
- `Host` 必须恰好一个且非空。
- `Content-Length` 最多一个，只接受十进制；`+1`、`-1`、`1.0`、`1,1` 拒绝。
- Content-Length 与 Transfer-Encoding 同时出现返回 400；任何单独
  Transfer-Encoding 返回 501；Expect 返回 417。
- Content-Length 数字溢出或超过 body 上限返回 413，解析前不做无界预分配。

## 3. 已生效的 `HttpLimits` 默认值

这些是库对象的默认硬限制；尚未接入 `AppConfig`：

| 项目 | 默认值 | 规则 |
|---|---:|---|
| method | 32 bytes | token，超限 fail-closed |
| target | 8 KiB | 超限 414 |
| 请求行 | 16 KiB | 包含结尾 CRLF；为 method + target + version 留出空间 |
| 单个 header 行 | 8 KiB | 包含结尾 CRLF；超限 400/431 后关闭 |
| header 总大小 | 32 KiB | 包含每行 CRLF 和终止空行；超限 431 后关闭 |
| header 数量 | 100 | 超限 431 |
| body | 1 MiB | 超限 413 |
| response body | 1 MiB | 序列化前拒绝，返回 ResourceExhausted |
| routes | 256 | 满时拒绝注册 |
| 每轮 requests | 16 | 超出后通过 EventLoop 普通有界队列继续 |

`HttpLimits::create` 接受有符号值，所有项必须大于 0，byte/count 有公开硬上限；
request line、method/target 和 header line/total 做跨字段校验，不静默 clamp。
`max_header_line_bytes`、`max_header_bytes` 与 `max_header_count` 同时约束请求和响应；
请求 total 从第一个 header 字节计至终止空行，响应 total 还包含状态行和自动 framing。
若响应限制小到连框架标准错误响应也无法容纳，`serialize()` 在发送任何字节前失败，
Session 按 fail-closed 策略关闭连接；不会发送缺少 `Content-Length`/`Connection`
的降级响应，也不会发送部分 Header。当前固定错误 `Content-Type` 行含 CRLF 为
41 字节，边界测试明确覆盖该精确容量。

### 3.1 Parser 状态与错误映射

```text
RequestLine -> Headers -> Body -> Complete
      \           \         \
                       -> Error (terminal)
Complete --take_request--> RequestLine
```

`parse` 返回 NeedMore、Complete 或 Error，并返回本次实际消费字节数；CRLF、请求行、
header 和 body 均可跨输入。Session 每轮立即从 TCP Buffer retrieve 已消费字节，
request 完成后的剩余字节留给下一条 pipeline。

| 情况 | 状态 |
|---|---:|
| 语法、Host、任意重复 header、CL+TE | 400 |
| body/CL 溢出或超限 | 413 |
| target/request line 超限 | 414 |
| Expect | 417 |
| header line/total/count 超限 | 431 |
| Transfer-Encoding、Upgrade | 501 |
| 非 HTTP/1.1 | 505 |
| 内部分配/状态失败 | 500（Session fail-closed） |

Parser Error 是终止态；Session 丢弃后续字节，至多生成一个错误响应并关闭。
当一个请求同时触发多项协议错误时，检查顺序固定为：header 语法/重复/Host 和
CL+TE 歧义优先 400，随后单独 Transfer-Encoding/Upgrade 为 501、Expect 为 417，
最后 Content-Length 溢出或 body 超限为 413；实现不依赖无序容器迭代顺序。

### 3.2 Response、Router 与会话

- `HttpResponse` 验证 header name/value，禁止 handler 设置 Content-Length、
  Transfer-Encoding 或 Connection；序列化前同时检查 body、header 数量、每个
  header 行和整个 head。计数与字节数包含自动生成的 Content-Length、Connection、
  状态行、每行 CRLF 和终止空行，失败不分配或返回部分响应字符串。
- `HttpRouter` 只做精确 method + path 匹配；query 不参与；freeze 后只读。
  404 保持连接，405 含排序稳定的 Allow。handler Error、标准/未知异常都转换为
  不泄露内部文本的 500 并关闭。
- `HttpSession` 每连接一个，不拥有 TcpConnection；请求/响应按顺序同步执行。
  每轮最多 `max_requests_per_dispatch`，同时至多一个普通 continuation；queue 满、
  Session terminal 或连接不再 Connected 时都不会继续 dispatch。
- `HttpServer` 拥有 frozen Router、TcpServer 和 connection-id→Session 表，不拥有
  EventLoop/Logger；TcpServer 继续拥有 TcpConnection，callback 捕获 weak server。
  断连后的 Session 表清理由 TcpServer 的内部 `DeferredCleanup` 触发，不依赖普通
  pending queue 的剩余容量，也不在 active Channel 批次内直接销毁对象。
- HTTP close 与传输层半关闭分离：`close_after_write()` 拒绝后续 send，排空现有
  输出后主动完成全关闭并触发一次 close callback，不等待 peer EOF；`shutdown()`
  仍表示写半关闭后等待 peer EOF 的传输层契约。
- `/health` 只表示 HTTP/EventLoop 可响应，不代表任务、插件、数据库、GPU 或工业
  设备 healthy。

## 4. 通用 JSON 约定

- 字段名使用 `snake_case`。
- 时间使用 UTC RFC 3339，例如 `2026-07-29T07:30:00.123Z`。
- `task_id`、`request_id` 是服务端生成的不透明字符串，客户端不得推导格式。
- 未知字段策略：
  - 顶层任务 envelope 默认拒绝，防止拼写错误被忽略；
  - `input` 是插件专属对象，由插件校验。
- JSON number 必须在目标类型范围内；不接受 `NaN`/`Infinity`。
- API 不回显服务端路径、堆栈、errno 或插件异常原文。

### 4.1 错误 envelope

```json
{
  "error": {
    "code": "INVALID_JSON",
    "message": "request body is not valid JSON",
    "request_id": "req_opaque"
  }
}
```

可选 `details` 只能包含安全、稳定、可机器处理的字段，例如：

```json
{
  "error": {
    "code": "VALIDATION_FAILED",
    "message": "request validation failed",
    "request_id": "req_opaque",
    "details": {
      "field": "plugin",
      "reason": "required"
    }
  }
}
```

## 5. 任务资源模型

### 5.1 状态

外部字符串：

- `queued`
- `running`
- `success`
- `failed`
- `cancelled`
- `timeout`

终态是 `success`、`failed`、`cancelled`、`timeout`。内部状态可命名为 `Succeeded`，JSON v1 仍稳定序列化为 `success`。

### 5.2 任务快照

```json
{
  "task_id": "task_opaque",
  "plugin": "mock_vision",
  "task_type": "weld_detect",
  "status": "running",
  "progress": 0.5,
  "created_at": "2026-07-29T07:30:00.123Z",
  "started_at": "2026-07-29T07:30:00.130Z",
  "finished_at": null,
  "deadline_at": "2026-07-29T07:30:30.130Z"
}
```

`progress` 范围 `[0.0, 1.0]`。插件不报告进度时为 `null`，不得伪造线性进度。`deadline_at` 在 queued 时为 `null`，进入 running 后按 `started_at + timeout_ms` 计算；首版 timeout 只计算执行时间，不包含有界队列等待时间。

## 6. API

### 6.1 GET /health

用途：轻量 liveness/readiness 初版合并检查。

成功：

```http
HTTP/1.1 200 OK
Content-Type: application/json

{"status":"ok"}
```

首版不在 health 中执行插件推理或阻塞式依赖探测。

### 6.2 POST /api/v1/tasks

提交异步任务。

请求：

```json
{
  "plugin": "mock_vision",
  "task_type": "weld_detect",
  "input": {
    "point_cloud_path": "data/sample.pcd",
    "weld_type_hint": "straight"
  },
  "timeout_ms": 30000
}
```

约束：

- `plugin`：必填，1—64 字符，建议只允许小写字母、数字、`_`、`-`。
- `task_type`：必填，1—64 字符，同样限制字符集。
- `input`：必填 JSON object，具体 schema 由插件校验。
- `timeout_ms`：可选正整数，表示进入 running 后的执行超时；省略时用服务端默认值，超过服务端最大值返回 422。
- 顶层不接受客户端提供 `task_id`、状态、结果或时间字段。

接受：

```http
HTTP/1.1 202 Accepted
Location: /api/v1/tasks/task_opaque
```

```json
{
  "task_id": "task_opaque",
  "status": "queued"
}
```

重要错误：

- 400：JSON 或 envelope 非法。
- 404：插件未注册或 disabled。
- 422：插件输入、task_type 或 timeout 语义错误。
- 503：任务队列/仓储满、线程池拒绝或服务停止中。

如果排队未被接受，不返回 task ID，也不留下可查询的孤儿任务。

工作队列满是服务容量不足，固定使用 503，不能包装为插件执行失败。429 只保留给未来独立的用户级请求限流。

### 6.3 GET /api/v1/tasks/{task_id}

返回任务快照。

成功：200，body 使用 5.2 的结构。

未知 task ID：

```json
{
  "error": {
    "code": "TASK_NOT_FOUND",
    "message": "task was not found",
    "request_id": "req_opaque"
  }
}
```

由于内存仓储会清理过期终态任务，404 既可能表示不存在，也可能表示已超过保留期。

### 6.4 GET /api/v1/tasks/{task_id}/result

成功任务：

```json
{
  "task_id": "task_opaque",
  "status": "success",
  "result": {
    "mock": true,
    "plugin": "mock_vision",
    "detected": true,
    "weld_type": "straight",
    "start_point": [0.0, 0.0, 0.0],
    "end_point": [100.0, 0.0, 0.0],
    "confidence": 0.95
  }
}
```

`MockVisionPlugin` 的 `detected`、类型、点位和置信度均为模拟字段，`mock: true` 不能被配置关闭。

非终态：

- HTTP 409
- 错误码 `TASK_NOT_FINISHED`
- details 可包含当前 `status`

失败任务：

```json
{
  "task_id": "task_opaque",
  "status": "failed",
  "error": {
    "code": "PLUGIN_EXECUTION_FAILED",
    "message": "plugin execution failed"
  }
}
```

计划中的超时任务响应拟使用 HTTP 504 和状态 `timed_out`。Phase 5 没有 Cancelled
状态；若未来增加取消 API，必须先扩展状态机和协议测试，再评估 409/410 语义。

### 6.5 POST /api/v1/plugins/{plugin_name}/execute

这是提交任务的便利路由，不提供同步、长连接阻塞式执行。

请求：

```json
{
  "task_type": "echo",
  "input": {
    "message": "hello"
  },
  "timeout_ms": 5000
}
```

路径中的 `plugin_name` 等价于 `/api/v1/tasks` body 中的 `plugin`。响应、排队和错误语义完全复用任务提交接口，避免两套执行模型。

## 7. 插件输入示例

### 7.1 EchoPlugin

支持 task type：`echo`。

输入为任意 JSON object，输出：

```json
{
  "echo": {
    "message": "hello"
  },
  "plugin": "echo"
}
```

Echo 用于框架验证，不代表工业算法。

### 7.2 MockVisionPlugin

支持 task type：`weld_detect`。

输入：

```json
{
  "point_cloud_path": "data/sample.pcd",
  "weld_type_hint": "straight"
}
```

首版行为：

- `point_cloud_path` 只作为经过长度、NUL、绝对路径和 `..` 校验的模拟标识，不打开文件。
- `weld_type_hint` 只允许文档列出的 mock 枚举。
- 可根据服务端配置模拟有限延迟。
- 输出始终包含 `mock: true` 和 `plugin: "mock_vision"`。

输出：

```json
{
  "mock": true,
  "plugin": "mock_vision",
  "detected": true,
  "weld_type": "straight",
  "start_point": [0.0, 0.0, 0.0],
  "end_point": [100.0, 0.0, 0.0],
  "confidence": 0.95
}
```

## 8. HTTP 解析错误策略

| 输入 | 行为 |
|---|---|
| 分段但合法的 request line/header/body | 缓存并等待更多字节 |
| 裸 LF、非法 header 名、header 无冒号 | 400，关闭 |
| 重复且值不同的 Content-Length | 400，关闭 |
| Content-Length 负数/非十进制 | 400，关闭 |
| Content-Length 数字溢出或超过 body 上限 | 413，关闭 |
| 同时有 Transfer-Encoding 和 Content-Length | 400，关闭 |
| chunked / 其他 Transfer-Encoding | 501，关闭 |
| body 超限 | 尽早 413，关闭，不继续缓存 body |
| JSON 解析失败 | 400，可按 keep-alive 策略关闭 |
| 未知路由 | 404 |
| 已知路径但方法错误 | 405，包含 Allow |
| peer 半关闭且请求完整 | 处理并写完后关闭 |
| peer 半关闭且请求不完整 | 关闭，不等待更多数据 |

错误请求后关闭连接是首版的保守策略，可避免解析器状态污染。

## 9. 原始 TCP JSON 扩展（planned）

未来若启用 TCP 任务协议，建议使用：

```text
4-byte unsigned big-endian payload_length
payload_length bytes UTF-8 JSON
```

不用换行分隔，避免 JSON 内换行歧义。长度先与配置上限比较，再分配/缓存。消息 envelope 复用 HTTP 服务层：

```json
{
  "version": 1,
  "request_id": "client_optional_id",
  "operation": "submit_task",
  "payload": {
    "plugin": "echo",
    "task_type": "echo",
    "input": {}
  }
}
```

该协议必须使用独立 `ProtocolCodec`/监听配置，不能在同一连接上猜测 HTTP 或二进制协议。版本协商、认证和流量控制在实现前另行设计。

## 10. 版本兼容

- URI 中固定 `/api/v1`。
- 在 v1 内只增加可选响应字段，不改变字段意义。
- 破坏性 schema 变更使用 `/api/v2` 或插件结果独立 schema version。
- 插件结果由插件负责版本字段；核心只保证 JSON envelope。

## 11. Phase 5 Task Runtime 协议边界

Phase 5 只定义进程内 C++ API：

- `TaskRequest` 只含通用 `operation` 与 `input` JSON，不含 HTTP request、
  TcpConnection、Socket 或插件对象。
- `TaskId` 由服务端仓库生成，稳定文本形态为 `task-` 加最少 16 位十进制数字；
  rollback/erase 后不复用，耗尽 `uint64_t` 后永久返回 ResourceExhausted；当前没有
  公开字符串解析入口。
- `TaskState` 只含 `queued`、`running`、`succeeded`、`failed`、`timed_out`。
- queue/repository/JSON 容量失败返回项目 `ErrorCode::ResourceExhausted`；
  not found 返回 `ErrorCode::NotFound`。Phase 5 不把它们映射为 HTTP 状态码。
- `mark_timed_out` 是未来定时器的接入点，不表示自动超时已实现。
- `erase_terminal` 可删除 Succeeded/Failed/TimedOut；删除 TimedOut 不终止仍在
  运行的 handler，晚到完成得到 NotFound 并被计数丢弃。
- 同一 TaskHandler 可被多个 worker 并发调用；调用者负责线程安全，且 handler
  不得依赖 EventLoop owner thread 或操作网络对象。
- `TaskManager::shutdown` 先关闭 admission，再等待所有 in-flight submit 完整提交
  或回滚，最后 drain/join；它不会自动删除终态记录。

第 6、7 节中的任务与插件 JSON 均继续属于 planned schema。只有后续阶段显式实现并
测试 Router/TaskManager 适配后，才能把这些文档结构描述为可访问 API。
