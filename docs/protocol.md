# 协议设计

## Phase 10D public WeldAgent result projection

Phase 10C is committed at
`32f6a269301e971d2588b034fcb7242d92b3a4e2` and passed Linux CI run
`31263414316`. Phase 10D completed separate local real-browser PTV2 and
WeldAgent flows. PTV2 used 2048 input points, returned 205 weld points, ratio
`0.10009765625`, length about `0.8822024465 mm`, a grey/red viewer and three
verified downloads while keeping `quality_assessment=not_implemented`.
WeldAgent used an independent 823114-point requested `straight` input, reached
`waiting_human`, displayed camera/mm start/end/path, RGB axes and confidence
without a corner, and kept `robot_execution_allowed=false`.

The public WeldAgent download protocol is a fixed allowlist projection created
from a validated IAISF Domain Result. The external `final_result.json` is used
only for controlled status evaluation; no external object or array is copied
or recursively sanitized. Optional validated fields are omitted when absent.
The verified file is 628 bytes with SHA-256
`71f49be2ecc3dc22c7ff49cd2cd6285e9b98a0ce1deb72d58728ed59f9001bf4` and
contains no joint, tcp, path, URL, command, log or unknown external field.
This local browser evidence is distinct from GitHub CI. The applications remain
independent; quality scoring, joint values, robot control and automatic chaining
are not protocol capabilities. Ignored evidence, inputs, local configuration,
models and outputs are not committed.

Final local validation recorded Windows Debug/Release at 692 registered, 687
passed, 5 explicitly skipped and 0 failed, and WSL Debug/Release at 925
registered, 924 passed, 1 explicitly skipped and 0 failed. Adapter 19/19, Web
UI 7/7 and Artifact HTTP 9/9 passed in all four configurations; Linux Service
Web UI routes passed 4/4 in Debug and Release.

## 1. 范围与状态

Phase 4 HTTP 协议库状态保持 `PHASE_4_HTTP_PROTOCOL_COMPLETED`。Phase 5 总体状态为
`PHASE_5_TASK_RUNTIME_COMPLETED`。Phase 7 当前为
`PHASE_7_SERVICE_INTEGRATION_COMPLETED`。可移植
`iaisf_http_core` 已在 Windows Debug/Release 各通过 84/84 HTTP Core 测试；
最终 [Linux CI run 30539245789](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30539245789)
的 Debug/Release 均为 239/239，其中 HTTP Core 84/84、Linux-only
HttpSession/HttpServer integration 16/16。Phase 5 最终
[Linux CI run 30547126540](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30547126540)
的 Debug/Release 均为 324/324，其中 Task Runtime 85/85；该结果不改变当前 HTTP
端点集合。Phase 6 功能提交 `66a606bb53bf8ed80b8efd6faf7c6529b5cd22d1` 的首次
[Linux CI run 30602538268](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30602538268)
Debug/Release 均为 428/428，其中 Task Runtime 97/97、Plugin System 92/92；
Release smoke 成功，但每个配置各有 3 条项目源码和 3 条项目测试 warning，因此
不是最终封板证据。warning 修复提交
`853ccccca80cdc042b3d51eae52fe45566aa2b22` 的最终
[Linux CI run 30604428624](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30604428624)
在 Debug/Release 各通过 428/428，项目源码和测试 warning 均为 0，完成 Phase 6
封板。

当前 CLI 不启动监听；`/health`、`/version` 是显式注册到 `HttpRouter` 后由
`HttpServer` API 提供的能力。Task Runtime 和 Plugin System 不依赖 HTTP，后文任务
Phase 7 已提供可组合的 C++ Task HTTP API；当前 CLI 仍不启动 `/v1/tasks`、
`/api/v1/tasks` 或 `/v1/plugins` 路由，CLI 也不加载插件。
HTTP Core 本身不提供通用静态目录、任意路径读取、shell 或客户端代码执行。
当 `applications.enabled=true` 时，Service 仅注册受控的编译内 Web UI、Artifact
上传和 catalog 校验下载路由；不存在任意文件服务器。

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

### 6.2 POST /api/v1/tasks（未来规划，Phase 7 未实现此路径）

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
  "status_url": "/api/v1/tasks/task_opaque"
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

进程内 operation：`echo`。

输入必须且只能包含 `payload`，payload 可为任意 JSON value：

```json
{
  "payload": {
    "message": "hello"
  }
}
```

输出：

```json
{
  "message": "hello"
}
```

Echo 直接返回 payload 的独立副本，不添加 operation 包装；operation 已在
TaskSnapshot 中记录。Echo 仅用于框架验证，不代表工业算法。

### 7.2 MockVisionPlugin

进程内 operation：`mock_vision.detect`。

输入：

```json
{
  "image_id": "demo-001",
  "width": 640,
  "height": 480,
  "confidence_threshold": 0.5
}
```

Phase 6 行为：

- `image_id` 是受 byte limit 和控制字符约束的标识，不是路径；未知/path 字段拒绝。
- width/height 是 1—16384 的严格整数；threshold 默认为 0.5，范围 `[0,1]`。
- 固定 mock confidence 为 0.93，bbox 只按输入尺寸确定性生成。
- 输出始终包含 `mock: true` 和 `operation: "mock_vision.detect"`。
- 不读取图片/点云、不运行模型/GPU，不代表准确率或真实性能。

输出：

```json
{
  "mock": true,
  "operation": "mock_vision.detect",
  "image_id": "demo-001",
  "image_size": {
    "width": 640,
    "height": 480
  },
  "detections": [
    {
      "type": "weld_seam",
      "confidence": 0.93,
      "bbox": {
        "x": 160,
        "y": 160,
        "width": 320,
        "height": 48
      }
    }
  ]
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
- Phase 5 实现提交为 `79d3d4e89feb71595dc67d820f9a5398dcc814d4`；最终 Linux
  Debug/Release 均实际执行 Task Runtime 85/85，项目源码和测试 warning 为 0。

第 6、7 节中的任务与插件 JSON 均继续属于 planned schema。只有后续阶段显式实现并
测试 Router/TaskManager 适配后，才能把这些文档结构描述为可访问 API。

## 12. Phase 6 进程内插件协议边界

- `TaskRequest.operation` 是 canonical plugin operation；内置值为 `echo` 与
  `mock_vision.detect`。
- PluginManager 在 Configuring 时只允许 register/freeze；list、lookup、validate 和
  execute 均返回 InvalidState 且不调用插件。freeze 幂等，Frozen 后目录稳定可并发读。
- TaskManager 先执行通用 TaskLimits，再调用可选 TaskValidator；unknown operation、
  schema error 或 validation exception 都在 TaskId 分配前失败。
- Task/Plugin 输入分别受序列化 bytes、depth、elements 和 string/key bytes 限制；
  discarded、nlohmann non-JSON binary、非法 UTF-8 与非有限浮点数 fail-closed。
  null/array/string/number/bool 在通用层允许，具体 operation schema 再由插件决定。
- 序列化 bytes 与 nlohmann/json 紧凑 `dump()` 精确一致，包括键、引号、转义、
  braces/brackets、comma/colon、数字文本和 UTF-8 实际字节；计数不保存完整 dump，
  首个超限字节会中止序列化。
- PluginTaskAdapter handler 在 worker execute 前再次校验已拥有的 TaskRequest
  快照；若同一插件的第二次校验结果变化，则固定 InternalError 且 execute 不会运行。
- Adapter 生成的 validator/handler 只捕获 `shared_ptr<const PluginManager>`，不保活
  Adapter 或 TaskManager；Manager 强持有 Plugin，整个所有权图无强引用环。
- 成功 submit 后调用方修改或销毁原 operation/input 不影响 Repository 或 worker。
- PluginManager 的 NotFound、InvalidArgument、ResourceExhausted 与泛化
  InternalError 都是 C++ `ErrorCode`；所有消息受 PluginLimits byte limit，本阶段
  未实现 HTTP 状态映射。
- `PluginManager::execute` 对直接 C++ 调用也先验证输入。插件成功输出在返回或写入
  Repository 前统一检查；超限输出为 ResourceExhausted，discarded/非法 UTF-8/
  非有限输出为 InternalError，均使任务 Failed 且不保存半结果。
- 插件异常、内部路径、errno、返回的内部 Error 文本和 `what()` 不进入 TaskSnapshot。
- Plugin System API 可由 C++ 组合进 TaskManager，但当前 CLI、HttpRouter 和
  HttpServer 都没有组合它；不得把第 6、7 节示例描述成已部署端点。

## 13. Phase 7 Task HTTP API（已实现，等待 Linux CI）

已实现的组合服务路径：

- `POST /v1/tasks`
- `GET /v1/tasks/{id}`
- `GET /health`
- `GET /version`

`POST /v1/tasks` 只接受根对象且只允许 `operation` 与 `input`；Content-Type 必须是
`application/json` 或唯一的 UTF-8 charset 参数。成功返回 202，TaskId 始终是字符串。
`GET /v1/tasks/{id}` 只接受 `task-` 加 16 位规范十进制形式，不接受符号、短形式、
前导格式变体、溢出或额外路径段。查询只返回 id、operation、state，以及终态对应的
result 或安全 error；input、时间戳、线程和异常原文不会出现在响应。

稳定映射：语法/schema/TaskId 为 400，unknown operation/task 为 404，Content-Type
为 415，插件输入校验为 422，输入超限为 413，queue/repository capacity 或停止中为
503，未分类内部失败为泛化 500。Router 404/405 在该组合中也使用 JSON，405 保留
`Allow`。所有 JSON 响应先完整序列化和容量验证，再交给 HTTP 层。

该建议不包含 timerfd、自动任务超时、signalfd、生产 CLI 常驻模式、动态插件、
GPU/真实 AI、数据库、异步日志或 benchmark。

### 13.1 Phase 7B 最终协议约束

- 202 body 仅为 `{"task_id":"...","status_url":"..."}`，不含 `state:"queued"`；
  accepted 与 TaskState 是不同概念。
- Failed task 是已存在资源的安全快照，因此 GET 返回 200 + `state:"failed"` + 固定
  `internal_error`，不含 result 或插件原始文本。
- stop 开始后 POST 为稳定 503 `service_stopping`；在 HttpServer 真正停止前，既有任务
  GET 继续允许。queue/repository capacity 分别为 503 `queue_full` /
  `repository_full`，均来自 typed submit outcome。
- TaskId 使用统一 canonical parser/formatter：`task-` 前缀、最少 16 位十进制，完整
  `uint64_t` 范围最长 20 位数字；符号、空白、大小写、非唯一前导零和溢出拒绝。
- terminal parameter route 只消费一个非空末段；query 不参与 path 匹配，额外 segment
  为 404，POST 到 status resource 为 405 并携带 `Allow: GET`。
- `/health` 保持 `{"status":"ok"}`，只说明当前 HTTP/EventLoop 路径响应，不表示
  GPU、模型、数据库、动态插件或 task capacity ready。

## Phase 7E Service 集成审计

最新 Linux CI run 30779555703（提交 `1cc332b9d9e02ae78ec9e43455d36ffe939f73e2`）在 Ubuntu 24.04.4/GCC 13.3.0/CMake 3.31.6 上 Debug、Release 均为 `497/497`，version/config smoke 成功。`IndustrialAiServiceTest.ActiveHttpStopWaitsForDeferredCleanupBeforeJoiningTasks` 两种配置均通过。

Active HTTP 请求触发 stop 时，当前连接可能在响应生成前由 TcpServer/HttpServer cleanup 关闭，停止触发请求不保证返回 503，也不能截断已开始发送的响应；普通 Stopping 阶段新到的 POST 仍返回 503。生命周期顺序固定为 `TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`，DeferredCleanup 先于阻塞任务 shutdown。

历史记录：该 run 当时因项目测试 `tests/service/test_industrial_ai_service.cpp:1041:51` 的 `-Wshadow` warning 未满足 warning=0；后续提交已修复。尚未执行 `ctest --repeat until-fail:50`；HTTP Task API 的路由和错误映射虽已集成到 Linux Service 测试，但 CLI 仍不启动常驻 HTTP 服务。

## Phase 7G 最终协议与生命周期验证

最终 push [Linux CI run 30781932731](https://github.com/realme-max/IndustrialAIServiceFramework/actions/runs/30781932731) 对提交 `a44b1272bf603a17724fa17c66d60ee0e18bb918` 完成 Debug/Release `497/497`，version/config smoke 成功，项目源码与测试 warning 均为 0。Task HTTP API 和 Service 生命周期测试已实际执行。

Active HTTP stop 语义保持：触发 stop 的请求不保证返回 503，连接可能在响应生成前关闭，已开始发送的 response 不得截断；普通 Stopping 阶段 POST 仍为 503。生命周期为 `TcpServer cleanup → HttpServer stopped → DeferredCleanup → TaskManager shutdown/join → Service Stopped`。Phase 7 状态为 `PHASE_7_SERVICE_INTEGRATION_COMPLETED`；尚未执行 `ctest --repeat until-fail:50`。

## Phase 9B-3A-2 Strict Application JSON Contract

This is a contract-only layer, not an HTTP protocol or route. Strict JSON is
preflighted with bounded SAX state before DOM construction. Duplicate object
keys, malformed UTF-8, comments, non-finite numbers, trailing bytes and
configured depth/node/text/payload limits fail closed.

The two version `1.0` roots are intentionally fixed: inspection has exactly
one point-cloud artifact and one or both requested outputs; guidance has one
artifact, `auto` or requested weld type and `human_checkpoint=required`.
Unknown fields, duplicate fields, paths, URLs, controller commands, joint
values and automatic execution fields are rejected. The parsed result contains
validated Domain values rather than source JSON. Status projection is bounded,
uses epoch milliseconds and emits only public identity/state/version/time and
the canonical application status URL. Existing Task API parsing is unchanged.
The fixed `xyz-f32le` artifact contract uses exactly 12 wire bytes per point
(three binary32 coordinates), with overflow-safe multiplication and the
existing 1 GiB artifact limit. Status projection catches allocation and
standard serialization failures and returns a bounded structured `Result`
error; it does not emit HTTP headers.

## Phase 9B-3A-3A Job ID and clock boundary

This phase adds no wire endpoint or JSON field. `OsApplicationJobIdGenerator`
produces only canonical candidate IDs (`wi_`/`wg_` plus 32 lowercase hex
characters) from 128 bits of OS CSPRNG entropy. The entropy reduces candidate
collision probability; Repository `create()` is the final authority for
process-local successful Job uniqueness, and 9B-3A-3A does not implement
collision retry. Job IDs are not authorization credentials. It does not perform
Repository admission, HTTP mapping or Service orchestration. The clock
interface returns a validated `system_clock::time_point`; pre-epoch and
unrepresentable Unix-millisecond values are failures. The two independent
business applications remain isolated and no PTV2/WeldAgent workflow is
introduced.
## Fast Track MVP-1 boundary

No network protocol is introduced by the local artifact/result domain. The
Python importer and C++ resolver communicate through the versioned
`ArtifactRef` manifest and fixed `xyz-f32le` bytes only. A future Worker or HTTP
contract must be versioned independently and must not expose filesystem paths,
controller commands, joint values or quality claims.

## Phase 10A Artifact HTTP contract

Artifact upload/download is a separate, enabled-only API:

```text
POST /api/artifacts/v1/pointclouds
GET  /api/artifacts/v1/files/{artifact_id}
```

The POST body is direct `text/plain` (optional charset) XYZ text. Non-empty
rows require three finite coordinates; extra columns are ignored. The service
stores little-endian float32 XYZ (`12 bytes/point`), `point_count`, SHA-256,
and an exact eight-field `artifact.json`. First creation returns `201`; an
identical validated duplicate returns `200`. Invalid syntax is `400`,
non-finite or non-representable float32 is `422`, unsupported media type is
`415`, request limit is `413`, and storage conflicts never return idempotent
success.

GET accepts only catalog-registered IDs. It rechecks canonical containment,
non-symlink regular-file status, exact size and the SHA-256 of the exact body
that will be returned. It emits the registered media type, a server-generated
safe filename, `X-Content-Type-Options: nosniff` and `Cache-Control: no-store`.
The framework hard HTTP byte ceiling is 64 MiB; this endpoint is whole-body
only and has no Range, streaming, authentication or persistence.

Every public output Artifact JSON includes the canonical
`/api/artifacts/v1/files/{artifact_id}` URL. PTV2 and WeldAgent remain
independent; PTV2 quality is `not_implemented` and WeldAgent never permits
robot execution.

Phase 10A local real HTTP evidence uses two independent historical inputs.
The PTV2 upload returned `201` for 2048 points and its job completed
`202 -> Succeeded -> 200`; all three output downloads matched their result
ArtifactRef sizes and SHA-256 values. The WeldAgent upload returned `201` for
823114 points and its requested `straight` job completed
`202 -> WaitingHuman -> 200`; `final_result.json` matched its ArtifactRef.
These are local external-process results, not GitHub Actions evidence, and
the earlier 2047-point sample remains a documented negative diagnostic.

## Phase 9 Fast Track MVP-3 Application HTTP contract

Six fixed versioned routes are available when the application runtime is
enabled:

```text
POST /api/weld-inspection/v1/jobs
GET  /api/weld-inspection/v1/jobs/{job_id}
GET  /api/weld-inspection/v1/results/{job_id}
POST /api/welding-guidance/v1/jobs
GET  /api/welding-guidance/v1/jobs/{job_id}
GET  /api/welding-guidance/v1/results/{job_id}
```

POST uses the strict application JSON contract and returns `202` with only
`job_id` and canonical `status_url`. Status progresses through `Accepted`,
`Queued`, `Dispatching`, `Running` and then `Succeeded`, `WaitingHuman` or
`Failed`. Result GET returns `200` only for `Succeeded` or `WaitingHuman`;
incomplete jobs return `409 result_not_ready`, failed jobs return
`409 job_not_succeeded`, invalid IDs return `400`, and unknown or
cross-application IDs return `404`.

PTV2 returns `quality_assessment=not_implemented`. WeldAgent may return draft
start/end/corner/axes and waiting metadata, but always returns
`robot_execution_allowed=false`. Neither route exposes joint values,
controller URLs, local paths, commands, stderr or tool configuration. The two
applications are independent and are never automatically chained. Persistence,
cancel/retry and remote workers remain out of scope for this historical MVP-3
contract; Phase 10A subsequently added the separate enabled-only Artifact
upload/download API documented above.

## Phase 10B same-origin Web UI

When applications are enabled, the service registers only these fixed same-
origin resources before router freeze:

```text
GET /
GET /ui/app.css
GET /ui/point-cloud-viewer.js
GET /ui/app.js
```

The four resources are compiled into the binary and use `text/html`,
`text/css` and `application/javascript` content types. They set `no-store`,
`nosniff`, `no-referrer`, `X-Frame-Options: DENY` and a strict self-only CSP;
there is no directory file server, CORS, CDN, external font or inline script.
Application-related route capacity is therefore 12 (six job routes, two
Artifact routes and four UI routes). The browser sends the existing direct
text upload, submits only the strict eight-field ArtifactRef, uses bounded
AbortController polling and treats `waiting_human` as a readable terminal
result. PTV2 and WeldAgent are never chained; quality remains
`not_implemented` and robot execution remains false. Three-dimensional
rendering is provided by the compiled Phase 10C viewer resource below. Current
evidence is limited to C++ resource/route contract tests; no real browser or
WebGL runtime E2E is claimed.

The Phase 10B client validates the upload response's exact eight-field
ArtifactRef before submission, validates the returned business-specific Job ID
and canonical status URL, and retains `artifact.point_count` for inspection
rendering. It bounds error text, maps malformed HTTP errors to status-specific
generic messages, and never inserts server data with `innerHTML`. A visible
stop-wait control aborts browser fetch/polling only; the server job may continue.
Result downloads are deduplicated across `output_artifacts`, `weld_points` and
`prediction` and require the canonical artifact URL. Native browser file
selection is supported; host-file access restrictions in automation are a
test-tool limitation. The Phase 10C viewer is a separate compiled resource and
does not change these HTTP or Application contracts.

## Phase 10C browser 3D visualization MVP

The MVP adds only the compiled-in `/ui/point-cloud-viewer.js` resource and
does not change Artifact or Application JSON. It decodes the validated
`xyz-f32le` input and fixed ASCII PTV2 PLY subset, keeps `prediction.txt`
download-only, and uses canonical Artifact URLs. It renders straight/corner
`start -> end` and L `start -> corner -> end` paths. Direction axes use
`start` as a display anchor only after finite, near-unit and near-orthogonal
validation; this is not an algorithmic axis origin. 3D failure falls back to
text results and downloads. PTV2 remains `quality_assessment=not_implemented`
and WeldAgent remains `robot_execution_allowed=false`.
