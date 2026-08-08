# Fast Track MVP-3

## Checkpoint

The local implementation checkpoint is commit
`b2f16cb99bc2e4c04cdf777fc6acc56575b35b16` (`feat: integrate industrial
application HTTP runtime`). This document records the current result without
claiming remote GPU or external-project execution.

## Implemented path

```text
strict application JSON
        |
        v
ApplicationHttpApi
        |
        v
ApplicationJobRepository -> ApplicationExecutor -> bounded single worker
        |                              |
        |                              +--> PTV2 post-weld adapter
        |                              +--> WeldAgent pre-weld adapter
        |
        +--> status/result JSON
```

The runtime validates local artifact metadata and manifest/SHA references,
materializes private point-cloud input, invokes an explicitly configured
executable without a shell, captures bounded output, registers regular output
files as hashed artifacts and cleans task-private scratch input. PTV2 and
WeldAgent are independent application paths and are never chained.

The six routes are:

```text
POST /api/weld-inspection/v1/jobs
GET  /api/weld-inspection/v1/jobs/{job_id}
GET  /api/weld-inspection/v1/results/{job_id}
POST /api/welding-guidance/v1/jobs
GET  /api/welding-guidance/v1/jobs/{job_id}
GET  /api/welding-guidance/v1/results/{job_id}
```

The application state sequence is `Accepted -> Queued -> Dispatching ->
Running -> Succeeded/WaitingHuman/Failed`, committed with Repository version
checks. Service shutdown closes application admission, stops HTTP/TCP cleanup,
drains the application worker and then continues the existing plugin shutdown
order.

## Local real HTTP evidence

PTV2 completed `202 -> Succeeded -> 200`: 2048 input points, 205 weld points,
weld ratio `0.10009765625`, length approximately `0.8822024465`, three output
artifacts, and `quality_assessment=not_implemented`.

WeldAgent completed `202 -> WaitingHuman -> 200` using the configured local
`tool_paths.local.json`: finite start/end/axes, camera/mm metadata, confidence,
and a bounded waiting reason. It returned
`robot_execution_allowed=false`; no joint values or controller command was
used. These are local E2E results only, not GitHub Actions evidence.

## Explicit non-goals

The checkpoint does not implement persistent Repository storage, HTTP Artifact
upload/download or a general Artifact Store, cancel, retry, heartbeat, lease,
fencing, remote Worker Protocol, PTV2 quality assessment, WeldAgent joint
values/trajectory/controller execution, automatic application chaining, or
real GPU/external-project E2E on GitHub runners.

## CI boundary

Linux CI explicitly builds the real application core, artifact/result,
repository, contract, API primitives and runtime targets plus their tests,
alongside the existing HTTP, Task, Plugin and Service targets. CI runs framework
tests and version/config smoke only; it does not receive external local paths,
models, CUDA libraries or local tool configuration.

The repository does not define an `iaisf_application_local_runtime` target;
the actual local runtime target is `iaisf_application_runtime`, and that name
is what the workflow verifies rather than inventing a failing alias.
