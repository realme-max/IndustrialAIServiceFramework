# Fast Track MVP-1: Local Artifact and Result Domain

Status: `PHASE_9_FAST_TRACK_MVP_1_LOCAL_ARTIFACT_AND_RESULT_DOMAIN_COMPLETED` (local, uncommitted).

This checkpoint adds a deliberately local, C++17 application layer. It does
not add HTTP routes, Worker Protocol, persistence, uploads, downloads,
deletion, TTL, Service composition, or PTV2/WeldAgent adapters.

## Artifact path

`tools/import_pointcloud.py` accepts a trusted local text file, consumes the
first three finite numeric columns of each non-empty line, and writes little
endian IEEE-754 binary32 XYZ records (`12 bytes/point`). Labels and trailing
columns are ignored and never enter the artifact. Output is staged through a
temporary file and atomically renamed into:

```text
<root>/inputs/<artifact_id>/pointcloud.xyzf32le
<root>/inputs/<artifact_id>/artifact.json
```

The importer computes SHA-256, byte size and point count, rejects empty,
non-finite, malformed and oversized input, refuses symlink roots/sources and
does not overwrite an existing artifact. The manifest is an exact
`ArtifactRef` projection.

`LocalArtifactResolver` accepts only a previously validated `ArtifactRef` and
derives the fixed path from its artifact id. It rejects symlinks, non-regular
files, size/manifest mismatches and SHA-256 mismatches before returning a path.

## Results and state invariants

`ApplicationExecutionResult` is a variant of `WeldInspectionResult` and
`WeldingGuidanceResult`. Inspection results always expose
`quality_assessment: "not_implemented"`; no confidence value is presented as
a quality score. Guidance results validate `straight`, `corner` and `l`
geometry, use millimetres, and always expose `robot_execution_allowed: false`.
They never contain joint values, controller URLs or remote commands.

`ApplicationJobSnapshot::completed()` is the only domain completion path:
inspection results produce `Succeeded`; guidance results produce `Succeeded`
or `WaitingHuman`. Non-final snapshots cannot carry a result, and resuming a
`WaitingHuman` snapshot clears that result. The Repository `complete()` method
validates state, application, version and result in one mutex transaction;
failed completion leaves the record unchanged.

The result serializer is a bounded C++ projection only. It emits no HTTP
headers and does not register a route. Input artifact paths and input hashes
are not projected; output references contain only bounded artifact metadata.

## Scope boundary

`weld_inspection/post_weld` and `welding_guidance/pre_weld` remain independent
applications. This phase does not connect them, start a worker, call a model,
execute a robot, or alter Service/Task/Plugin behavior.
