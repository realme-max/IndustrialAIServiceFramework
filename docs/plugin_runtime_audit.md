# Dynamic Plugin Runtime Audit

Status: `PHASE_8G_FINAL_DYNAMIC_PLUGIN_HARDENED` (local implementation audit;
no commit or release CI run has been created in this worktree).

## End-to-end ownership

```text
AppConfig
  -> RuntimeOptions
  -> DynamicPluginLoader
  -> DynamicModule (native handle)
  -> C ABI negotiation and validation
  -> DynamicPluginAdapter (instance + module)
  -> PluginRuntime (transactional registry and execution lease)
  -> PluginTaskAdapter / TaskManager
  -> HTTP Task API
```

`AppConfig` owns only portable values. `RuntimeOptions` owns validated platform
paths. `DynamicPluginLoader` owns no published plugin; it returns an adapter
that retains its `DynamicModule`. `PluginRuntime` publishes the adapter only
after metadata validation and initialization succeed. The `PluginExecutionLease`
holds both the plugin and runtime/entry observation state, so a module cannot
be released while validation or execution is active.

## Startup and rollback

Service creation runs on the EventLoop owner thread and performs static
registration, dynamic loading, dynamic registration, and `freeze()` before
constructing or starting HTTP. A load, ABI, metadata, initialization,
duplicate-operation, capacity, or HTTP construction error returns before the
Service is published. Local `shared_ptr` ownership then destroys adapters and
releases native handles; the signal-owner rollback guard restores the signal
queue. There is no partially started Service.

## Adapter lifecycle and error matrix

```text
Prepared -> Created -> Initialized -> Draining -> Stopped
       \-> Failed       \----------------------^ 
```

- create failure: no instance is published;
- initialize failure: instance is destroyed, ABI shutdown is not called;
- execute error/exception: mapped to a bounded error, runtime remains usable;
- shutdown error/exception: recorded, destroy is still attempted exactly once;
- destroy error/exception: caught and mapped, instance pointer is invalidated;
- native unload: `DynamicModule::close()` clears the handle even when the
  platform reports failure; the runtime records
  `plugin_dynamic_unload_failures_total` and returns a bounded shutdown error.

No C ABI callback exception crosses the boundary and no worker is terminated by
plugin code. Dynamic modules are released only after all leases drain.

## Metrics and diagnostics

All metrics are fixed, label-free names. Runtime counters/gauges include
registration, initialization, validation, execution, shutdown and active lease
state. Dynamic loading exposes:

- `plugin_dynamic_modules_loaded` (gauge);
- `plugin_dynamic_load_failures_total` (counter);
- `plugin_dynamic_unload_failures_total` (counter).

Metrics are optional and best effort: a missing registry or a wrong-type
pre-existing metric does not affect plugin execution. `/debug/status` exposes
only copied operation/version/origin/module-id/state/count fields. It never
returns a library path, root, config, native handle, input/output payload, or
exception text, and its response-size limit remains fail-closed.

## Safe path and platform audit

The resolver requires a canonical directory and regular file below it. It
rejects absolute, drive, UNC, `..`, `.`, empty-component, backslash,
control-character, invalid-UTF-8, symlink and reparse-point paths. Linux and
Windows platform-library selection is explicit; there is no extension guessing,
directory scan, PATH search, or remote source. Permission tests report an
explicit skip when the host cannot enforce a restricted directory (for example,
root or an ACL policy that ignores POSIX mode bits); the skip is never silent.

## Fixture and evidence

The real CMake MODULE fixture exercises ABI create, initialize, validate,
execute, shutdown, destroy and module release without AI dependencies, threads
or sleeps. In-process fake ABI tests deterministically cover create failure,
initialize failure, execute exception, shutdown exception/status failure and
destroy exception. Existing symlink/reparse tests are platform-gated with an
explicit reason when the OS refuses fixture creation.

Final local validation after this hardening is recorded separately from GitHub
Actions. WSL Ubuntu 24.04 Debug and Release each ran `761/761` with one
explicit permission-capability skip and zero failures. Windows VS2022 Debug
and Release each registered 533 tests, passed 528, skipped five explicit
environment cases, and failed zero. Project source and test compiler warnings
were zero. The existing workflow builds `iaisf_plugin`, `iaisf_plugin_tests`,
the dynamic loader/adapter test targets, fixture modules and the Service
target; no workflow change is required for target coverage. No new remote run
exists for this uncommitted worktree. ASan/UBSan were not run because no
sanitizer configuration is enabled locally.
