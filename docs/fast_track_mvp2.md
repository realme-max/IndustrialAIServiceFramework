# Fast Track MVP-2 - Controlled Runner and Real Adapters

Status: `PHASE_9_FAST_TRACK_MVP_2_CONTROLLED_RUNNER_AND_REAL_ADAPTERS_COMPLETED`
locally; the MVP-2 commit is local and has not been pushed.

This phase adds a narrow application-runtime layer without HTTP, Service
composition, Repository mutation, Worker Protocol, or automatic chaining of
the two industrial applications.

- `Ptv2WeldInspectionAdapter` is only for `weld_inspection/post_weld`.
- `WeldAgentWeldingGuidanceAdapter` is only for `welding_guidance/pre_weld`.
- Both adapters receive a validated `ApplicationJobSnapshot` and server-owned
  options, return `Result<ApplicationExecutionResult>`, and never mutate a
  Repository.
- PTV2 results keep `quality_assessment=not_implemented`; segmentation ratio,
  confidence, and point counts are not quality decisions.
- WeldAgent results always set `robot_execution_allowed=false` and do not
  expose joint values or controller fields.

`ProcessSpec` carries executable, argument vector, working directory, timeout,
and bounded stdout/stderr limits separately. `LocalProcessRunner` uses native
parameterized process creation (`CreateProcessW` on Windows and `fork`/`execv`
with pipes on Linux); no shell, `system`, or `popen` is used. Timeouts
terminate the child and output limits fail closed.

`PointCloudTxtMaterializer` accepts only resolver-verified
`application/vnd.iaisf.pointcloud.xyz-f32le`, decodes exactly 12 little-endian
bytes per point, rejects non-finite coordinates, and writes three-column text
under a job-private scratch directory. `LocalOutputArtifactRegistrar` accepts
only regular non-symlink files below a configured output root, recomputes
SHA-256, and writes a manifest.

The PTV2 repository was inspected read-only. The generic materializer remains
three-column `x y z`. The PTV2 adapter creates a private atomic bridge in the
same job scratch directory and writes `x y z 0` after validating exactly three
finite values per row and matching the Artifact point count. The fourth column
is only the existing PTV2 loader compatibility placeholder: it is not ground
truth, is not used as a model feature, and is never used for quality
assessment. The bridge and generic input are removed after process completion
or failure. An archived `weld_trt_demo.exe`, engine, plugin and sample were
then executed through `Ptv2WeldInspectionAdapter + LocalProcessRunner`.
WeldAgent smoke also uses the independent adapter and no automatic chaining is
implemented.

## Local validation

- Windows VS2022 Debug: 657 registered, 652 passed, 5 explicit capability
  skips, 0 failed.
- Windows VS2022 Release: 657 registered, 652 passed, 5 explicit capability
  skips, 0 failed.
- WSL Ubuntu 24.04 Release: 886 registered, 885 passed, 1 explicit
  permission-capability skip, 0 failed.
- WSL Ubuntu 24.04 Debug: the new runtime target ran 9/9 focused tests.
- Runtime tests were 9/9 in each Windows and WSL focused run. Python importer
  tests were 2/2.

These are local results, not GitHub Actions evidence. Project source and test
compiler warnings were zero; WSL emitted only shared-tree GNU make clock-skew
diagnostics. The adapter-mediated PTV2 smoke used the archived executable,
production plan/plugin and the same 2048-point XYZ data. It exited 0 with 205
weld points, weld ratio 0.10009765625, length 0.8822024465 and three registered
output Artifacts (`weld_result.json`, `weld_points.ply`, `prediction.txt`).
`quality_assessment` remained `not_implemented` and scratch input cleanup was
verified. A direct WeldAgent pointcloud smoke exited 0 and produced
`final_result.json`; its adapter requires human review and keeps
`robot_execution_allowed=false`.
