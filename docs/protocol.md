# 协议设计

## 1. 范围与状态

本文定义目标协议。Phase 1 只实现配置验证 CLI，没有 Socket、HTTP parser 或端点；本文件中的所有网络 API 仍为 planned。

首个可用协议是 HTTP/1.1 + UTF-8 JSON。原始 TCP JSON 仅保留扩展设计，不在 Phase 1—5 的首要验收范围。服务不提供 HTML、文件下载、任意路径读取、shell 或客户端代码执行。

## 2. HTTP 基线

- 支持方法：GET、POST。
- 支持版本：HTTP/1.1。
- 请求体：`application/json`；无 body 的 GET 除外。
- body framing：只支持 `Content-Length`。
- keep-alive：HTTP/1.1 默认开启，`Connection: close` 后关闭。
- 不支持：HTTPS、chunked、multipart、HTTP/2、WebSocket。
- 所有响应都设置 `Content-Type: application/json; charset=utf-8` 和准确的 `Content-Length`。
- 服务端生成 `X-Request-Id`；若将来允许接收客户端 request ID，必须先做长度和字符集校验。

## 3. 建议默认限制

这些是设计默认值，Phase 7 配置实现前不视为已生效：

| 项目 | 建议默认值 | 规则 |
|---|---:|---|
| 请求行 | 4 KiB | 超限 400/414 后关闭 |
| 单个 header 行 | 8 KiB | 超限 400/431 后关闭 |
| header 总大小 | 32 KiB | 超限 431 后关闭 |
| header 数量 | 100 | 超限 431 |
| body | 1 MiB | 超限 413 |
| 每连接输入缓冲硬上限 | 2 MiB | 超限关闭 |
| 每连接输出缓冲硬上限 | 2 MiB | 超限关闭 |
| 每连接连续请求数 | 100 | 达到后响应 `Connection: close` |
| 默认任务超时 | 30 s | 受最大值限制 |
| 最大任务超时 | 5 min | 客户端不能绕过 |

实际值必须由配置加载并在启动时校验。

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
Content-Type: application/json; charset=utf-8

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

超时任务返回 HTTP 504 和状态 `timeout`。Cancelled 返回 409；若未来增加取消 API，可再评估 410 语义。

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
| Content-Length 溢出/负数/非十进制 | 400，关闭 |
| 同时有 Transfer-Encoding 和 Content-Length | 400，关闭 |
| chunked | 501 或 400，固定一种实现并测试，关闭 |
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
