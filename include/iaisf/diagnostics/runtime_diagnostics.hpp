#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/health/health_checker.hpp"
#include "iaisf/logging/log_diagnostics.hpp"
#include "iaisf/metrics/metrics.hpp"
#include "iaisf/plugin/plugin_runtime.hpp"
#include "iaisf/task/task_manager.hpp"

namespace iaisf::diagnostics {

struct TaskDiagnostics {
    bool accepting{false};
    bool stopped{true};
    std::size_t repository_size{0U};
    std::size_t pending_count{0U};
    std::size_t task_exceptions{0U};
    std::size_t late_completions{0U};
    std::size_t handler_exceptions{0U};
    std::size_t logger_failures{0U};
};

struct LoggerDiagnostics {
    bool available{false};
    LogDiagnosticsState state{LogDiagnosticsState::Stopped};
    std::uint64_t accepted{0U};
    std::uint64_t filtered{0U};
    std::uint64_t dropped{0U};
    std::uint64_t rejected_after_shutdown{0U};
    std::uint64_t sink_failures{0U};
};

struct PluginRuntimeDiagnosticsSnapshot {
    bool available{false};
    bool dynamic_loading_enabled{false};
    std::size_t dynamic_module_count{0U};
    plugin::PluginRuntimeState state{plugin::PluginRuntimeState::Stopped};
    std::size_t registered_count{0U};
    std::size_t active_executions{0U};
    std::size_t managed_plugins{0U};
    bool shutdown_failed{false};
    std::vector<plugin::PluginEntrySnapshot> entries;
};

struct RuntimeDiagnosticsSnapshot {
    health::HealthStatus health;
    TaskDiagnostics tasks;
    LoggerDiagnostics logger;
    PluginRuntimeDiagnosticsSnapshot plugins;
    MetricsSnapshot metrics;
};

/** Read-only aggregation owned by the service composition root. */
class RuntimeDiagnostics final {
public:
    [[nodiscard]] static Result<std::shared_ptr<RuntimeDiagnostics>> create(
        std::shared_ptr<const health::HealthChecker> health_checker,
        const task::TaskManager& task_manager,
        const MetricsRegistry& metrics,
        const ILogDiagnostics* logger = nullptr,
        std::weak_ptr<const plugin::PluginRuntime> plugin_runtime = {});

    RuntimeDiagnostics(
        std::shared_ptr<const health::HealthChecker> health_checker,
        const task::TaskManager& task_manager,
        const MetricsRegistry& metrics,
        const ILogDiagnostics* logger,
        std::weak_ptr<const plugin::PluginRuntime> plugin_runtime) noexcept;

    [[nodiscard]] Result<RuntimeDiagnosticsSnapshot> snapshot() const;

private:
    std::shared_ptr<const health::HealthChecker> health_checker_;
    const task::TaskManager& task_manager_;
    const MetricsRegistry& metrics_;
    const ILogDiagnostics* const logger_{nullptr};
    std::weak_ptr<const plugin::PluginRuntime> plugin_runtime_;
};

/** Produces bounded JSON without exposing task payloads or log text. */
[[nodiscard]] Result<std::string> to_json(
    const RuntimeDiagnosticsSnapshot& snapshot,
    std::size_t maximum_bytes);

class DiagnosticsJsonAdapter final {
public:
    [[nodiscard]] static Result<std::string> encode(
        const RuntimeDiagnosticsSnapshot& snapshot,
        std::size_t maximum_bytes) {
        return to_json(snapshot, maximum_bytes);
    }
};

} // namespace iaisf::diagnostics
