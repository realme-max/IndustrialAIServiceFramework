# Application Layer

## Phase 10A Artifact Web API hardening

When `applications.enabled=true`, the service owns one bounded,
thread-safe `LocalArtifactCatalog` shared by upload, output registration and
download. Upload accepts direct `text/plain` XYZ rows, ignores blank lines and
columns after XYZ, and commits canonical `xyz-f32le` bytes plus an exact
eight-field manifest using a deterministic `pc_<sha256>` ID. Existing data and
manifest content are revalidated before a duplicate can return `200`; a
missing manifest may be repaired, while malformed or conflicting metadata is a
storage conflict. Uploads are serialized per API instance so identical
concurrent content commits once.

Downloads are whole-body, bounded by `HttpLimits::max_response_body_bytes()`.
The file is read once into the response body, hashed from those exact bytes,
and checked again for size/path/type before returning. Symlinks, root escapes,
replacement, truncation, growth and digest mismatch fail closed. No Range,
streaming, authentication or persistent catalog is provided; the catalog is
process-local and is empty after restart.

The final local real HTTP E2E uses separate inputs for the two applications.
PTV2 uploads a 2048-point artifact, reaches `Succeeded`, returns 205 weld
points and three output artifacts, and each download byte count/SHA matches
the result projection. WeldAgent uploads an independent 823114-point artifact,
reaches `WaitingHuman` for requested straight weld with finite start/end/axes
and confidence, and its `final_result.json` download matches its SHA. The
applications are not chained; PTV2 quality remains
`quality_assessment=not_implemented` and robot execution remains disabled.

## 当前阶段

Phase 9A 已完成只读设计审计。Phase 9B domain/repository、Fast Track MVP-1/2
以及 MVP-3 本地运行时接入均已完成。MVP-3 增加本地点云导入、SHA/manifest 与
Artifact resolver、受控跨平台进程、独立 PTV2/WeldAgent adapters、Application
Executor、单 worker 有界队列和六条 versioned HTTP route。

PTV2 与 WeldAgent 是完全独立的 `weld_inspection/post_weld` 和
`welding_guidance/pre_weld` 应用，不自动串联。本地真实 HTTP E2E 已通过；远端 CI
不执行真实 GPU/外部项目 E2E。持久化 Repository、通用 Artifact Store、取消/重试、
远程 Worker Protocol、质量评价、机器人控制和跨应用自动串联仍未实现。

## 独立应用边界

两个应用是完全独立的产品流程，不自动串联：

| Application | Scene phase | 当前业务边界 |
|---|---|---|
| `weld_inspection` | `post_weld` | 未来可接入 PTV2 的焊缝/背景分割和几何提取；当前没有独立质量算法，必须报告 `quality_assessment=not_implemented` |
| `welding_guidance` | `pre_weld` | 未来可接入 WeldAgent 的检测与人工复核流程；不得生成或宣称真实 joint values，不得控制机器人或发送 controller URL |

交叉组合一律 fail-closed。PTV2 不是 WeldAgent 的上游或下游，Phase 9B-2 也不包含
二者的 adapter 或 worker。

## ApplicationJobId 与 Snapshot

`ApplicationJobId` 是大小写敏感的 opaque 强类型：长度 1–64 ASCII bytes，首字符为
字母或数字，后续只允许字母、数字、`_` 和 `-`。它拒绝空白、控制字符、NUL、非 ASCII、
`.`、斜杠、反斜杠、盘符、UNC 和 URL；Repository 不生成、重写或规范化 ID。

ID 使用显式 copy-preserving move：移动构造和移动赋值复制底层值，源对象和目标对象都
保持相同合法 ID，不使用空字符串哨兵，也不依赖 moved-from `std::string` 的实现结果。
copy/move assignment 先构造完整副本，再通过无分配 `swap` 提交，因此分配失败时目标值不变。

`ApplicationJobSnapshot` 固定从 `Accepted`、version 1 开始，`created_at == updated_at`。
它持有 1–16 个经过 `validate_artifact_ref()` 的独立输入副本，并拒绝重复 `artifact_id`。
16 是当前保守的内存元数据硬上限，防止尚无 wire/store 层时出现无界 vector；后续协议
只能在不放宽该 Domain 上限的前提下增加更严格限制。时间由调用者显式传入，测试不依赖
系统时钟或等待。公开访问器只暴露 const 值视图，不暴露 Repository 内部可变引用。
Snapshot 采用相同的 copy-preserving move 与先复制后 swap 策略；移动后源 snapshot 仍含
合法 ID 和完整 artifact 集合，并可继续安全执行 `transitioned()`。create 和 transitioned
都会重新校验 ID、application/scene、state/version/time 与 artifact 集合的完整不变量。

## Application Job 状态机

状态集合为 `accepted`、`queued`、`dispatching`、`running`、`waiting_human`、
`cancelling`、`cancelled`、`succeeded`、`failed`、`timed_out`、`worker_lost`。
转换由 `validate_transition()` 集中裁决：

| From | Allowed to |
|---|---|
| Accepted | Queued, Cancelled, Failed |
| Queued | Dispatching, Cancelled, TimedOut, Failed |
| Dispatching | Running, Cancelling, Failed, TimedOut, WorkerLost |
| Running | Succeeded, Failed, TimedOut, WorkerLost, Cancelling；仅 Guidance 可进入 WaitingHuman |
| WaitingHuman | Running, Cancelled, TimedOut, Failed；仅 Guidance 可使用 |
| Cancelling | Cancelled, Failed, TimedOut, WorkerLost |
| terminal | 无 |

自转换、非法枚举和 inspection 的 `waiting_human` 均拒绝。终态不可恢复；未来重试需要
新的 attempt，不能把 `worker_lost` 改回 `running`。

## ArtifactRef

公共 `ArtifactRef` 只有值语义字段：`artifact_id`、`sha256`、`size_bytes`、`kind`、
`media_type`，以及可选 `coordinate_frame`、`unit`、`point_count`。它不包含也不解释
文件路径、storage key、UNC、URL、worker 路径、controller URL 或凭据，不检查文件系统，
也不根据扩展名推断媒体类型。

硬限制：

- artifact ID：1–128 ASCII bytes，首字符为字母或数字，其余只允许字母、数字、`_`、`-`、`.`；
- SHA-256：恰好 64 个小写十六进制字符；
- size：1 byte–1 GiB；
- kind：1–64 bytes；media type：1–128 bytes；
- coordinate frame：1–128 bytes；unit：1–32 bytes（仅在字段存在时校验）；
- point count：1–100,000,000（仅在字段存在时校验）；
- 文本字段拒绝 NUL、C0 控制字符和 DEL，错误消息稳定、有界且不回显输入。

未来点云 artifact 的首选媒体类型记录为
`application/vnd.iaisf.pointcloud.xyz-f32le`，但本阶段没有解析器，也没有把 PTV2 的
`x y z label` 评估输入固化为生产格式。

## Repository contract 与并发语义

`IApplicationJobRepository` 支持 `create`、按 application 隔离的 `get`、带
`expected_version` 的原子 `transition`、终态 `erase_terminal`、`size` 和 `capacity`。
失败通过 `ApplicationRepositoryFailure` 结构化区分 invalid argument、duplicate ID、
not found、capacity exceeded、version conflict、version exhausted、invalid transition、
invalid timestamp 和 internal failure；调用方不得解析错误文本。

`InMemoryApplicationJobRepository` 使用单 mutex 保护有界记录表。create/transition/erase
在一个临界区内完成检查和提交；失败不改变 size、state、version 或 timestamp。成功转换
调用 Phase 9B-1 的 `validate_transition()`，version 精确增加 1；`UINT64_MAX` 永久
fail-closed，不回绕。相同版本的并发更新只有一个提交成功，其余返回 `VersionConflict`。
返回值均为独立 snapshot。ID 存在但 application 不匹配与未知 ID 都返回 `NotFound`。
所有 Repository ID 入口均先调用 `ApplicationJobId::valid()`：语法无效 ID 返回
`InvalidArgument` 且不触碰记录；该策略与合法 ID 的跨 application `NotFound` 隔离语义不同。

Repository 不启动线程、不触发回调、不做自动驱逐/TTL/重启恢复。终态只有携带精确版本
才能显式删除，删除只释放元数据容量，不访问、拥有或删除 Artifact 内容。

## 构建和所有权边界

`iaisf_application_core` / `iaisf::application_core` 是 C++17 portable static target，
仅 PUBLIC 依赖 `iaisf::core`。它不依赖 Reactor、TCP/HTTP、Service、TaskManager、
PluginRuntime、Threads、filesystem I/O、TensorRT、CUDA、Qt 或 Python。

`iaisf_application_repository` / `iaisf::application_repository` PUBLIC 依赖
`iaisf::application_core`，仅为内部 mutex 实现链接标准 Threads；它不依赖 HTTP、Reactor、
Service、Task、Plugin、JSON、filesystem、数据库或平台 API。Repository 由调用者独占持有，
没有后台线程和跨模块销毁顺序约束。

Domain 对象均为调用者持有的普通值；没有线程、后台活动、网络资源或销毁顺序约束。
返回 `Result` 的解析和验证函数需要在失败路径构造包含 `std::string` 的 `Error`，因此不
声明 `noexcept`；纯 enum/string 查询、`is_terminal` 和内部 bool 检查仍保持 `noexcept`。

## 后续边界

Phase 9B-3 建议只定义 versioned HTTP/JSON Application API，将稳定 Domain/Repository
错误映射到明确状态码，并保持 snapshot 输出有界；持久化、Artifact Store 与跨进程
Worker Protocol 仍应留在后续阶段。不得为了复用现有
`/v1/tasks` 将多阶段 WeldAgent 强行包装成 `IAlgorithmPlugin`，也不得在协议稳定前接入
真实 PTV2、WeldAgent、GPU 或机器人。

## Phase 9B-3A-1：Submission Specification

本阶段仅补齐 Domain submission specification。`ApplicationSubmissionSpec` 使用受控 factory
创建，内部保存 inspection 或 guidance 的已验证值；不保存原始 JSON、不执行 I/O，也不依赖
HTTP、线程或平台 API。

`InspectionRequestedOutputs` 使用 bitmask 表示输出集合，因此 segmentation/geometry 的输入
顺序不会形成两个不同的 Domain 值。至少一个输出必选，重复和非法枚举无法进入类型。

`WeldTypeRequest` 强制 `auto` 不带 requested type，`requested` 必须携带 `straight`、
`corner` 或 `l`。`HumanCheckpointPolicy` 当前只有 `required`。这些字段只是业务请求，
不代表质量评价、算法分派、IK、轨迹、碰撞检查或机器人执行。

Submission specification 已加入 Job request 和 immutable snapshot。Snapshot create、
transition 和 Repository create 均验证 application/scene/spec 三方匹配；copy/move、get、
transition 和终态删除不会丢失 specification。当前 Domain 仍允许 1–16 个 ArtifactRef；
后续 HTTP v1 会暂时限制为恰好一个点云输入，直到 artifact role 被正式定义。

本阶段没有 JSON/HTTP API、Task API/route、幂等、dispatcher、worker、Artifact I/O/Store、
AppConfig、RuntimeOptions 或 Service 组合；Job ID generator 和 clock 仅在后续
Phase 9B-3A-3A 的独立 primitives target 中实现。

Phase 9B-3A-3A 加固后的本地验证：Windows VS2022 Debug/Release 各注册 642 项（637 passed、5
explicitly skipped、0 failed），WSL Ubuntu 24.04 GCC Debug/Release 各注册 871 项（870
passed、1 explicitly skipped、0 failed）；Application label Windows 为 103/103、WSL 为
104/104，`iaisf_application_api_primitives_tests` Windows 为 18/18、Linux 为 19/19（含
1 个 Linux-only getrandom seam 测试）。项目源码/测试 warning 为 0；WSL
结果不代表 GitHub Actions。

## Phase 9B-3A-2 Strict Application JSON Contract

The contract layer is separate from the Application Domain and does not expose
nlohmann JSON types in Domain headers. It parses only two fixed version `1.0`
roots: post-weld inspection (`segmentation`/`geometry`) and pre-weld guidance
(`auto` or requested `straight`/`corner`/`l`) with a required human checkpoint.
Both require exactly one point-cloud artifact with exact metadata fields and
overflow-safe `size_bytes == point_count * 12` validation. The 12-byte value is
the fixed XYZ binary32 wire contract and is independent of host `sizeof(float)`.

The status projection emits only schema version, job identity, state, version,
epoch-millisecond timestamps and a canonical status URL. It intentionally does
not expose artifacts, hashes, submission specs, results, review/checkpoint
data, quality assessment or internal errors. This phase has no HTTP route and
does not migrate the existing Task API parser. Status URL construction, JSON
construction and serialization are contained by the public `Result` exception
boundary; allocation-bearing public functions do not claim `noexcept`.

## Phase 9B-3A-3A Job ID Generator and Clock

The stable Phase 9B-3A-2 base is commit `4d3284febd95907ecdf20f0b96aa2ab1f5044855`
(Linux CI run `31140290934`). The current Phase 9B-3A-3A worktree is local and
uncommitted.

`iaisf_application_api_primitives` depends publicly only on
`iaisf::application_core`. `OsApplicationJobIdGenerator` reads 128 bits from
the platform CSPRNG on every call: `BCryptGenRandom` on Windows and Linux
`getrandom` with flags zero, retrying EINTR and short reads. It encodes
`WeldInspection` as `wi_` and `WeldingGuidance` as `wg_`, followed by 32
lowercase hexadecimal bytes, then validates the final 35-byte value through
`ApplicationJobId::parse()`. No timestamp, client input, artifact hash,
collision retry, Repository access or singleton is involved. CSPRNG entropy
only reduces candidate collision probability; Repository `create()` is the
final authority for process-local successful Job uniqueness, 9B-3A-3A has no
collision retry, and Job IDs are not authorization credentials. Failure categories
are `InvalidApplication`, `EntropyUnavailable`, `ResourceFailure` and
`InternalFailure`, with bounded fixed messages.

`IApplicationJobClock` and `SystemApplicationJobClock` are independent of the
generator. The system clock is read once per call; pre-epoch and unrepresentable
Unix-millisecond values fail closed using checked integer ratio arithmetic, with
no floating-point boundary decision. Test-only deterministic entropy, clock and
Linux getrandom syscall seams are private to the test target and are not exposed
in installed public headers.
