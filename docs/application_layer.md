# Application Layer

## 当前阶段

Phase 9A 已完成只读设计审计。Phase 9B-1 只建立跨平台、无 I/O 的应用领域基础：
`ApplicationIdentity`、`ApplicationJobState` 和 `ArtifactRef`。HTTP Application API、
Repository、Artifact 存储及 Worker Protocol 均尚未实现。

## 独立应用边界

两个应用是完全独立的产品流程，不自动串联：

| Application | Scene phase | 当前业务边界 |
|---|---|---|
| `weld_inspection` | `post_weld` | 未来可接入 PTV2 的焊缝/背景分割和几何提取；当前没有独立质量算法，必须报告 `quality_assessment=not_implemented` |
| `welding_guidance` | `pre_weld` | 未来可接入 WeldAgent 的检测与人工复核流程；不得生成或宣称真实 joint values，不得控制机器人或发送 controller URL |

交叉组合一律 fail-closed。PTV2 不是 WeldAgent 的上游或下游，Phase 9B-1 也不包含
二者的 adapter 或 worker。

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

## 构建和所有权边界

`iaisf_application_core` / `iaisf::application_core` 是 C++17 portable static target，
仅 PUBLIC 依赖 `iaisf::core`。它不依赖 Reactor、TCP/HTTP、Service、TaskManager、
PluginRuntime、Threads、filesystem I/O、TensorRT、CUDA、Qt 或 Python。

本阶段对象均为调用者持有的普通值；没有线程、后台活动、网络资源或销毁顺序约束。
返回 `Result` 的解析和验证函数需要在失败路径构造包含 `std::string` 的 `Error`，因此不
声明 `noexcept`；纯 enum/string 查询、`is_terminal` 和内部 bool 检查仍保持 `noexcept`。

## 后续边界

下一小阶段建议先定义 versioned Application API contract 和独立的 ApplicationJob
Repository 接口，再设计 Artifact 存储与跨进程 Worker Protocol。不得为了复用现有
`/v1/tasks` 将多阶段 WeldAgent 强行包装成 `IAlgorithmPlugin`，也不得在协议稳定前接入
真实 PTV2、WeldAgent、GPU 或机器人。
