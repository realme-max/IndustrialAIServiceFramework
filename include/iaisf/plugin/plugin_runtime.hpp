#pragma once

#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "iaisf/core/result.hpp"
#include "iaisf/metrics/metrics.hpp"
#include "iaisf/plugin/algorithm_plugin.hpp"
#include "iaisf/plugin/plugin_manager.hpp"

namespace iaisf::plugin {

enum class PluginRuntimeState {
    Configuring,
    Frozen,
    Draining,
    Stopped,
    Failed,
};

/** Lifecycle state of one registered plugin entry. */
enum class PluginEntryState {
    Registered,
    Initializing,
    Ready,
    Draining,
    Stopped,
    Failed,
};

/** Read-only copy of one plugin's runtime lifecycle observation. */
struct PluginEntrySnapshot {
    std::string operation;
    PluginMetadata metadata;
    bool managed_lifecycle{false};
    PluginEntryState state{PluginEntryState::Registered};
    std::size_t active_execution_count{0U};
    bool shutdown_failed{false};
};

namespace detail {

struct PluginRuntimeEntry {
    std::string operation;
    PluginMetadata metadata;
    bool managed_lifecycle{false};
    PluginEntryState state{PluginEntryState::Registered};
    std::size_t active_executions{0U};
    bool shutdown_failed{false};
    std::shared_ptr<IManagedAlgorithmPlugin> lifecycle;
};

struct PluginRuntimeLeaseState {
    mutable std::mutex mutex;
    std::condition_variable changed;
    PluginRuntimeState state{PluginRuntimeState::Configuring};
    std::size_t active_executions{0U};
    std::size_t registrations_in_progress{0U};
    bool shutdown_in_progress{false};
    std::optional<Error> shutdown_error;
    std::shared_ptr<Gauge> active_metric;
    std::shared_ptr<Gauge> state_metric;
};

}  // namespace detail

/**
 * Move-only lifetime token for one plugin validation or execution.
 *
 * The token keeps both the plugin object and the runtime lease state alive.
 * Runtime shutdown waits for all tokens to be released before invoking
 * plugin shutdown hooks.
 */
class PluginExecutionLease final {
public:
    PluginExecutionLease() noexcept = default;
    ~PluginExecutionLease() noexcept;

    PluginExecutionLease(const PluginExecutionLease&) = delete;
    PluginExecutionLease& operator=(const PluginExecutionLease&) = delete;

    PluginExecutionLease(PluginExecutionLease&& other) noexcept;
    PluginExecutionLease& operator=(PluginExecutionLease&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(plugin_);
    }

    [[nodiscard]] const std::shared_ptr<const IAlgorithmPlugin>&
    plugin() const noexcept {
        return plugin_;
    }

private:
    friend class PluginRuntime;

    PluginExecutionLease(
        std::shared_ptr<detail::PluginRuntimeLeaseState> state,
        std::shared_ptr<const IAlgorithmPlugin> plugin,
        std::shared_ptr<detail::PluginRuntimeEntry> entry) noexcept;
    void release() noexcept;

    std::shared_ptr<detail::PluginRuntimeLeaseState> state_;
    std::shared_ptr<const IAlgorithmPlugin> plugin_;
    std::shared_ptr<detail::PluginRuntimeEntry> entry_;
};

/**
 * Owns the lifecycle of an in-process plugin registry.
 *
 * PluginRuntime is intentionally independent from TaskManager and HTTP. Its
 * closures may be borrowed by those layers, but the runtime never owns them.
 * Static plugins remain supported; dynamic loading is deliberately outside
 * this phase.
 */
class PluginRuntime final {
public:
    /**
     * Creates a runtime. The optional metrics registry is borrowed and must
     * outlive the returned runtime; passing nullptr disables instrumentation.
     */
    [[nodiscard]] static Result<std::shared_ptr<PluginRuntime>> create(
        PluginLimits limits,
        MetricsRegistry* metrics = nullptr);

    PluginRuntime(const PluginRuntime&) = delete;
    PluginRuntime& operator=(const PluginRuntime&) = delete;
    PluginRuntime(PluginRuntime&&) = delete;
    PluginRuntime& operator=(PluginRuntime&&) = delete;
    ~PluginRuntime() noexcept;

    [[nodiscard]] PluginRuntimeState state() const noexcept;

    /** Registers and initializes one static plugin transactionally. */
    [[nodiscard]] Result<void> register_plugin(
        std::shared_ptr<const IAlgorithmPlugin> plugin);
    [[nodiscard]] Result<void> register_static_plugin(
        std::shared_ptr<const IAlgorithmPlugin> plugin) {
        return register_plugin(std::move(plugin));
    }

    /** Freezes registration and enables concurrent invocation. */
    [[nodiscard]] Result<void> freeze();

    [[nodiscard]] bool frozen() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t active_execution_count() const noexcept;
    [[nodiscard]] const PluginLimits& limits() const noexcept;

    /** Returns a stable copy of one entry's lifecycle observation. */
    [[nodiscard]] Result<PluginEntrySnapshot> entry_snapshot(
        std::string_view operation) const;

    /** Returns all entry observations in operation order. */
    [[nodiscard]] Result<std::vector<PluginEntrySnapshot>>
    entry_snapshots() const;

    [[nodiscard]] Result<void> validate(
        std::string_view operation,
        const nlohmann::json& input) const;
    [[nodiscard]] Result<nlohmann::json> execute(
        std::string_view operation,
        const nlohmann::json& input) const;

    /**
     * Acquires a stable invocation token. Only Frozen runtimes issue tokens.
     */
    [[nodiscard]] Result<PluginExecutionLease> acquire_execution_lease(
        std::string_view operation) const;

    /**
     * Stops new runtime calls, waits for leases, then shuts down managed
     * plugins in reverse initialization order. Repeated calls are idempotent.
     */
    [[nodiscard]] Result<void> shutdown();

private:
    friend class PluginTaskAdapter;

    using Entry = detail::PluginRuntimeEntry;

    explicit PluginRuntime(PluginLimits limits, MetricsRegistry* metrics);

    void initialize_metrics() noexcept;
    void set_state_locked(PluginRuntimeState state) noexcept;
    void set_entry_state_locked(
        const std::shared_ptr<detail::PluginRuntimeEntry>& entry,
        PluginEntryState state) noexcept;

    [[nodiscard]] Result<void> validate_for_execution(
        std::string_view operation,
        const nlohmann::json& input) const;
    [[nodiscard]] Result<nlohmann::json> execute_validated(
        std::string_view operation,
        const nlohmann::json& input) const;
    [[nodiscard]] Error fixed_internal_error(
        std::string_view message) const;

    struct ManagedPlugin {
        std::shared_ptr<Entry> entry;
    };

    std::shared_ptr<PluginManager> manager_;
    std::shared_ptr<detail::PluginRuntimeLeaseState> lease_state_;
    std::map<std::string, std::shared_ptr<Entry>> entries_;
    std::vector<ManagedPlugin> managed_plugins_;
    PluginLimits limits_;
    MetricsRegistry* const metrics_{nullptr};
    std::shared_ptr<Counter> registrations_metric_;
    std::shared_ptr<Counter> registration_failures_metric_;
    std::shared_ptr<Counter> initializations_metric_;
    std::shared_ptr<Counter> initialization_failures_metric_;
    std::shared_ptr<Counter> validate_calls_metric_;
    std::shared_ptr<Counter> validate_failures_metric_;
    std::shared_ptr<Counter> execute_calls_metric_;
    std::shared_ptr<Counter> execute_success_metric_;
    std::shared_ptr<Counter> execute_failures_metric_;
    std::shared_ptr<Counter> shutdown_metric_;
    std::shared_ptr<Counter> shutdown_failures_metric_;
    std::shared_ptr<Gauge> registered_metric_;
    std::shared_ptr<Gauge> active_metric_;
    std::shared_ptr<Gauge> state_metric_;
    std::shared_ptr<Histogram> validate_duration_metric_;
    std::shared_ptr<Histogram> execute_duration_metric_;
};

}  // namespace iaisf::plugin
