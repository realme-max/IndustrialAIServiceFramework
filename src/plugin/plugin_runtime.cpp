#include "iaisf/plugin/plugin_runtime.hpp"

#include <chrono>
#include <exception>
#include <new>
#include <string>
#include <utility>

#include "iaisf/core/error.hpp"
#include "iaisf/plugin/plugin_metadata.hpp"

namespace iaisf::plugin {
namespace {

std::string bounded_message(
    const std::string_view message,
    const std::size_t maximum) {
    return std::string(message.substr(0, maximum));
}

Error fixed_error(
    const ErrorCode code,
    const std::string_view message,
    const std::size_t maximum) {
    return make_error(code, bounded_message(message, maximum));
}

template <typename Metric, typename Create, typename Get>
std::shared_ptr<Metric> metric_or_existing(
    Create&& create,
    Get&& get) noexcept {
    try {
        auto created = create();
        if (created) {
            return created.value();
        }
        auto existing = get();
        if (existing) {
            return existing.value();
        }
    } catch (...) {
        // Metrics are observational. Allocation/type failures must not
        // change plugin registration or execution behavior.
    }
    return {};
}

int state_value(const PluginRuntimeState state) noexcept {
    switch (state) {
    case PluginRuntimeState::Configuring:
        return 0;
    case PluginRuntimeState::Frozen:
        return 1;
    case PluginRuntimeState::Draining:
        return 2;
    case PluginRuntimeState::Stopped:
        return 3;
    case PluginRuntimeState::Failed:
        return 4;
    }
    return 4;
}

class RegistrationMetricsGuard final {
public:
    RegistrationMetricsGuard(
        std::shared_ptr<Counter> metric,
        bool& committed) noexcept
        : metric_(std::move(metric)), committed_(committed) {}

    ~RegistrationMetricsGuard() noexcept {
        if (!committed_ && metric_) {
            metric_->increment();
        }
    }

    RegistrationMetricsGuard(const RegistrationMetricsGuard&) = delete;
    RegistrationMetricsGuard& operator=(const RegistrationMetricsGuard&) = delete;

private:
    std::shared_ptr<Counter> metric_;
    bool& committed_;
};

double elapsed_seconds(
    const std::chrono::steady_clock::time_point started) noexcept {
    return std::chrono::duration<double>{
               std::chrono::steady_clock::now() - started}
        .count();
}

}  // namespace

PluginExecutionLease::PluginExecutionLease(
    std::shared_ptr<detail::PluginRuntimeLeaseState> state,
    std::shared_ptr<const IAlgorithmPlugin> plugin,
    std::shared_ptr<detail::PluginRuntimeEntry> entry) noexcept
    : state_(std::move(state)),
      plugin_(std::move(plugin)),
      entry_(std::move(entry)) {}

PluginExecutionLease::~PluginExecutionLease() noexcept {
    release();
}

PluginExecutionLease::PluginExecutionLease(
    PluginExecutionLease&& other) noexcept
    : state_(std::move(other.state_)),
      plugin_(std::move(other.plugin_)),
      entry_(std::move(other.entry_)) {}

PluginExecutionLease& PluginExecutionLease::operator=(
    PluginExecutionLease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        plugin_ = std::move(other.plugin_);
        entry_ = std::move(other.entry_);
    }
    return *this;
}

void PluginExecutionLease::release() noexcept {
    if (!state_) {
        plugin_.reset();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->active_executions > 0U) {
            --state_->active_executions;
        }
        if (entry_ && entry_->active_executions > 0U) {
            --entry_->active_executions;
        }
        if (state_->active_metric) {
            state_->active_metric->decrement();
        }
    }
    state_->changed.notify_all();
    state_.reset();
    plugin_.reset();
    entry_.reset();
}

Result<std::shared_ptr<PluginRuntime>> PluginRuntime::create(
    PluginLimits limits,
    MetricsRegistry* const metrics) {
    try {
        auto runtime = std::shared_ptr<PluginRuntime>{
            new PluginRuntime(std::move(limits), metrics)};
        runtime->initialize_metrics();
        return Result<std::shared_ptr<PluginRuntime>>::success(
            std::move(runtime));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<PluginRuntime>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin runtime"));
    } catch (const std::exception&) {
        return Result<std::shared_ptr<PluginRuntime>>::failure(make_error(
            ErrorCode::InternalError,
            "unable to construct plugin runtime"));
    } catch (...) {
        return Result<std::shared_ptr<PluginRuntime>>::failure(make_error(
            ErrorCode::InternalError,
            "unable to construct plugin runtime"));
    }
}

PluginRuntime::PluginRuntime(
    PluginLimits limits,
    MetricsRegistry* const metrics)
    : manager_(std::make_shared<PluginManager>(limits)),
      lease_state_(std::make_shared<detail::PluginRuntimeLeaseState>()),
      limits_(std::move(limits)),
      metrics_(metrics) {}

void PluginRuntime::initialize_metrics() noexcept {
    if (metrics_ == nullptr) {
        return;
    }

    registrations_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_registrations_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_registrations_total");
        });
    registration_failures_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_registration_failures_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_registration_failures_total");
        });
    initializations_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_initializations_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_initializations_total");
        });
    initialization_failures_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_initialization_failures_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_initialization_failures_total");
        });
    validate_calls_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_validate_calls_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_validate_calls_total");
        });
    validate_failures_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_validate_failures_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_validate_failures_total");
        });
    execute_calls_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_execute_calls_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_execute_calls_total");
        });
    execute_success_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_execute_success_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_execute_success_total");
        });
    execute_failures_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_execute_failures_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_execute_failures_total");
        });
    shutdown_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter("plugin_runtime_shutdown_total");
        },
        [this] {
            return metrics_->get_counter("plugin_runtime_shutdown_total");
        });
    shutdown_failures_metric_ = metric_or_existing<Counter>(
        [this] {
            return metrics_->create_counter(
                "plugin_runtime_shutdown_failures_total");
        },
        [this] {
            return metrics_->get_counter(
                "plugin_runtime_shutdown_failures_total");
        });
    registered_metric_ = metric_or_existing<Gauge>(
        [this] {
            return metrics_->create_gauge("plugin_runtime_registered");
        },
        [this] { return metrics_->get_gauge("plugin_runtime_registered"); });
    active_metric_ = metric_or_existing<Gauge>(
        [this] {
            return metrics_->create_gauge("plugin_runtime_active_executions");
        },
        [this] {
            return metrics_->get_gauge("plugin_runtime_active_executions");
        });
    state_metric_ = metric_or_existing<Gauge>(
        [this] { return metrics_->create_gauge("plugin_runtime_state"); },
        [this] { return metrics_->get_gauge("plugin_runtime_state"); });
    validate_duration_metric_ = metric_or_existing<Histogram>(
        [this] {
            return metrics_->create_histogram(
                "plugin_validate_duration_seconds");
        },
        [this] {
            return metrics_->get_histogram(
                "plugin_validate_duration_seconds");
        });
    execute_duration_metric_ = metric_or_existing<Histogram>(
        [this] {
            return metrics_->create_histogram(
                "plugin_execute_duration_seconds");
        },
        [this] {
            return metrics_->get_histogram(
                "plugin_execute_duration_seconds");
        });

    lease_state_->active_metric = active_metric_;
    lease_state_->state_metric = state_metric_;
    if (state_metric_) {
        state_metric_->set(state_value(PluginRuntimeState::Configuring));
    }
}

void PluginRuntime::set_state_locked(
    const PluginRuntimeState state) noexcept {
    lease_state_->state = state;
    if (state_metric_) {
        state_metric_->set(state_value(state));
    }
}

void PluginRuntime::set_entry_state_locked(
    const std::shared_ptr<detail::PluginRuntimeEntry>& entry,
    const PluginEntryState state) noexcept {
    if (entry) {
        entry->state = state;
    }
}

PluginRuntime::~PluginRuntime() noexcept {
    (void)shutdown();
}

PluginRuntimeState PluginRuntime::state() const noexcept {
    std::lock_guard<std::mutex> lock(lease_state_->mutex);
    return lease_state_->state;
}

Result<void> PluginRuntime::register_plugin(
    std::shared_ptr<const IAlgorithmPlugin> plugin) {
    bool committed = false;
    RegistrationMetricsGuard registration_guard{
        registration_failures_metric_, committed};
    if (!plugin) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin must not be null"));
    }

    std::unique_lock<std::mutex> lifecycle_lock(lease_state_->mutex);
    if (lease_state_->state != PluginRuntimeState::Configuring) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin runtime is not configuring"));
    }

    PluginMetadata metadata;
    try {
        metadata = plugin->metadata();
        auto valid = validate_metadata(metadata, limits_);
        if (!valid) {
            return Result<void>::failure(fixed_error(
                valid.error().code,
                valid.error().message,
                limits_.max_error_message_bytes()));
        }
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(fixed_internal_error(
            "plugin metadata allocation failed"));
    } catch (const std::length_error&) {
        return Result<void>::failure(fixed_internal_error(
            "plugin metadata exceeds the platform size limit"));
    } catch (const std::exception&) {
        return Result<void>::failure(fixed_internal_error(
            "plugin metadata failed"));
    } catch (...) {
        return Result<void>::failure(fixed_internal_error(
            "plugin metadata failed"));
    }

    auto slot = manager_->validate_registration_slot(metadata.operation);
    if (!slot) {
        return slot;
    }

    const auto plugin_for_registry = plugin;
    std::shared_ptr<IManagedAlgorithmPlugin> lifecycle;
    try {
        auto mutable_plugin = std::const_pointer_cast<IAlgorithmPlugin>(
            plugin);
        lifecycle = std::dynamic_pointer_cast<IManagedAlgorithmPlugin>(
            std::move(mutable_plugin));
    } catch (...) {
        return Result<void>::failure(fixed_internal_error(
            "plugin lifecycle inspection failed"));
    }

    std::shared_ptr<detail::PluginRuntimeEntry> entry;
    try {
        entry = std::make_shared<detail::PluginRuntimeEntry>();
        entry->operation = metadata.operation;
        entry->metadata = metadata;
        entry->managed_lifecycle = static_cast<bool>(lifecycle);
        entry->lifecycle = lifecycle;
        const auto previous = entries_.find(entry->operation);
        if (previous != entries_.end()) {
            if (previous->second->state == PluginEntryState::Failed &&
                previous->second->active_executions == 0U) {
                entries_.erase(previous);
            } else {
                return Result<void>::failure(fixed_error(
                    ErrorCode::InvalidState,
                    "plugin operation is already registered",
                    limits_.max_error_message_bytes()));
            }
        }
        const auto inserted = entries_.emplace(entry->operation, entry);
        if (!inserted.second) {
            return Result<void>::failure(fixed_error(
                ErrorCode::InvalidState,
                "plugin operation is already registered",
                limits_.max_error_message_bytes()));
        }
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(fixed_internal_error(
            "unable to allocate plugin runtime entry"));
    } catch (const std::exception&) {
        return Result<void>::failure(fixed_internal_error(
            "unable to create plugin runtime entry"));
    } catch (...) {
        return Result<void>::failure(fixed_internal_error(
            "unable to create plugin runtime entry"));
    }

    if (lifecycle) {
        set_entry_state_locked(entry, PluginEntryState::Initializing);
        ++lease_state_->registrations_in_progress;
        if (initializations_metric_) {
            initializations_metric_->increment();
        }
        bool initialization_failed = false;
        bool rollback_failed = false;
        std::optional<Error> initialization_error;
        lifecycle_lock.unlock();
        try {
            auto initialized_result = lifecycle->initialize();
            if (!initialized_result) {
                initialization_failed = true;
                if (initialization_failures_metric_) {
                    initialization_failures_metric_->increment();
                }
                initialization_error = fixed_error(
                    initialized_result.error().code,
                    initialized_result.error().message,
                    limits_.max_error_message_bytes());
            }
        } catch (const std::bad_alloc&) {
            initialization_failed = true;
            if (initialization_failures_metric_) {
                initialization_failures_metric_->increment();
            }
            initialization_error = fixed_internal_error(
                "plugin initialization allocation failed");
        } catch (const std::exception&) {
            initialization_failed = true;
            if (initialization_failures_metric_) {
                initialization_failures_metric_->increment();
            }
            initialization_error = fixed_internal_error(
                "plugin initialization failed");
        } catch (...) {
            initialization_failed = true;
            if (initialization_failures_metric_) {
                initialization_failures_metric_->increment();
            }
            initialization_error = fixed_internal_error(
                "plugin initialization failed");
        }
        if (initialization_failed) {
            try {
                rollback_failed = !lifecycle->shutdown();
            } catch (...) {
                rollback_failed = true;
            }
        }
        lifecycle_lock.lock();
        --lease_state_->registrations_in_progress;
        lease_state_->changed.notify_all();
        if (initialization_failed) {
            set_entry_state_locked(entry, PluginEntryState::Failed);
            entry->lifecycle.reset();
            if (rollback_failed) {
                entry->shutdown_failed = true;
                set_state_locked(PluginRuntimeState::Failed);
            }
            return Result<void>::failure(std::move(*initialization_error));
        }
    }

    if (lifecycle) {
        try {
            managed_plugins_.push_back(ManagedPlugin{entry});
        } catch (const std::bad_alloc&) {
            bool rollback_failed = false;
            try {
                rollback_failed = !lifecycle->shutdown();
            } catch (...) {
                rollback_failed = true;
            }
            set_entry_state_locked(entry, PluginEntryState::Failed);
            entry->lifecycle.reset();
            if (rollback_failed) {
                entry->shutdown_failed = true;
                set_state_locked(PluginRuntimeState::Failed);
            }
            return Result<void>::failure(fixed_internal_error(
                "plugin lifecycle registration allocation failed"));
        }
    }

    auto registered = manager_->register_plugin_with_validated_metadata(
        plugin_for_registry, std::move(metadata));
    if (!registered) {
        if (lifecycle) {
            managed_plugins_.pop_back();
            bool rollback_failed = false;
            try {
                rollback_failed = !lifecycle->shutdown();
            } catch (...) {
                rollback_failed = true;
            }
            set_entry_state_locked(entry, PluginEntryState::Failed);
            entry->lifecycle.reset();
            if (rollback_failed) {
                entry->shutdown_failed = true;
                set_state_locked(PluginRuntimeState::Failed);
            }
        } else {
            set_entry_state_locked(entry, PluginEntryState::Failed);
        }
        return registered;
    }
    set_entry_state_locked(entry, PluginEntryState::Ready);
    committed = true;
    if (registrations_metric_) {
        registrations_metric_->increment();
    }
    if (registered_metric_) {
        registered_metric_->increment();
    }
    return Result<void>::success();
}

Result<void> PluginRuntime::freeze() {
    std::lock_guard<std::mutex> lock(lease_state_->mutex);
    if (lease_state_->state == PluginRuntimeState::Frozen) {
        return Result<void>::success();
    }
    if (lease_state_->state != PluginRuntimeState::Configuring) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin runtime cannot be frozen"));
    }
    if (lease_state_->registrations_in_progress != 0U) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin registration is still initializing"));
    }
    auto result = manager_->freeze();
    if (!result) {
        set_state_locked(PluginRuntimeState::Failed);
        return result;
    }
    set_state_locked(PluginRuntimeState::Frozen);
    return Result<void>::success();
}

bool PluginRuntime::frozen() const noexcept {
    return state() == PluginRuntimeState::Frozen;
}

std::size_t PluginRuntime::size() const noexcept {
    return manager_->size();
}

std::size_t PluginRuntime::active_execution_count() const noexcept {
    std::lock_guard<std::mutex> lock(lease_state_->mutex);
    return lease_state_->active_executions;
}

const PluginLimits& PluginRuntime::limits() const noexcept {
    return limits_;
}

Result<PluginEntrySnapshot> PluginRuntime::entry_snapshot(
    const std::string_view operation) const {
    try {
        std::lock_guard<std::mutex> lock(lease_state_->mutex);
        auto valid = validate_plugin_operation(operation, limits_);
        if (!valid) {
            return Result<PluginEntrySnapshot>::failure(fixed_error(
                valid.error().code,
                valid.error().message,
                limits_.max_error_message_bytes()));
        }
        const auto found = entries_.find(std::string(operation));
        if (found == entries_.end()) {
            return Result<PluginEntrySnapshot>::failure(fixed_error(
                ErrorCode::NotFound,
                "plugin operation was not found",
                limits_.max_error_message_bytes()));
        }
        const auto& entry = *found->second;
        return Result<PluginEntrySnapshot>::success(PluginEntrySnapshot{
            entry.operation,
            entry.metadata,
            entry.managed_lifecycle,
            entry.state,
            entry.active_executions,
            entry.shutdown_failed});
    } catch (const std::bad_alloc&) {
        return Result<PluginEntrySnapshot>::failure(fixed_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin entry snapshot",
            limits_.max_error_message_bytes()));
    } catch (const std::exception&) {
        return Result<PluginEntrySnapshot>::failure(fixed_internal_error(
            "unable to create plugin entry snapshot"));
    } catch (...) {
        return Result<PluginEntrySnapshot>::failure(fixed_internal_error(
            "unable to create plugin entry snapshot"));
    }
}

Result<std::vector<PluginEntrySnapshot>> PluginRuntime::entry_snapshots()
    const {
    try {
        std::lock_guard<std::mutex> lock(lease_state_->mutex);
        std::vector<PluginEntrySnapshot> snapshots;
        snapshots.reserve(entries_.size());
        for (const auto& item : entries_) {
            const auto& entry = *item.second;
            snapshots.push_back(PluginEntrySnapshot{
                entry.operation,
                entry.metadata,
                entry.managed_lifecycle,
                entry.state,
                entry.active_executions,
                entry.shutdown_failed});
        }
        return Result<std::vector<PluginEntrySnapshot>>::success(
            std::move(snapshots));
    } catch (const std::bad_alloc&) {
        return Result<std::vector<PluginEntrySnapshot>>::failure(fixed_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin entry snapshots",
            limits_.max_error_message_bytes()));
    } catch (const std::exception&) {
        return Result<std::vector<PluginEntrySnapshot>>::failure(
            fixed_internal_error("unable to create plugin entry snapshots"));
    } catch (...) {
        return Result<std::vector<PluginEntrySnapshot>>::failure(
            fixed_internal_error("unable to create plugin entry snapshots"));
    }
}

Result<PluginExecutionLease> PluginRuntime::acquire_execution_lease(
    const std::string_view operation) const {
    std::unique_lock<std::mutex> lock(lease_state_->mutex);
    if (lease_state_->state != PluginRuntimeState::Frozen) {
        return Result<PluginExecutionLease>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin runtime is not frozen"));
    }
    auto plugin = manager_->find_plugin(operation);
    if (!plugin) {
        return Result<PluginExecutionLease>::failure(std::move(plugin).error());
    }
    const auto found_entry = entries_.find(std::string(operation));
    if (found_entry == entries_.end() || !found_entry->second) {
        return Result<PluginExecutionLease>::failure(fixed_internal_error(
            "plugin runtime entry was not found"));
    }
    const auto& entry = found_entry->second;
    if (entry->state != PluginEntryState::Ready) {
        return Result<PluginExecutionLease>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin entry is not ready"));
    }
    ++lease_state_->active_executions;
    ++entry->active_executions;
    if (lease_state_->active_metric) {
        lease_state_->active_metric->increment();
    }
    return Result<PluginExecutionLease>::success(PluginExecutionLease{
        lease_state_, std::move(plugin).value(), entry});
}

Result<void> PluginRuntime::validate(
    const std::string_view operation,
    const nlohmann::json& input) const {
    if (validate_calls_metric_) {
        validate_calls_metric_->increment();
    }
    const auto started = std::chrono::steady_clock::now();
    auto lease = acquire_execution_lease(operation);
    if (!lease) {
        if (validate_failures_metric_) {
            validate_failures_metric_->increment();
        }
        if (validate_duration_metric_) {
            validate_duration_metric_->record(elapsed_seconds(started));
        }
        return Result<void>::failure(std::move(lease).error());
    }
    auto result = manager_->validate(operation, input);
    if (!result && validate_failures_metric_) {
        validate_failures_metric_->increment();
    }
    if (validate_duration_metric_) {
        validate_duration_metric_->record(elapsed_seconds(started));
    }
    return result;
}

Result<nlohmann::json> PluginRuntime::execute(
    const std::string_view operation,
    const nlohmann::json& input) const {
    if (execute_calls_metric_) {
        execute_calls_metric_->increment();
    }
    const auto started = std::chrono::steady_clock::now();
    auto lease = acquire_execution_lease(operation);
    if (!lease) {
        if (execute_failures_metric_) {
            execute_failures_metric_->increment();
        }
        if (execute_duration_metric_) {
            execute_duration_metric_->record(elapsed_seconds(started));
        }
        return Result<nlohmann::json>::failure(std::move(lease).error());
    }
    auto valid = manager_->validate(operation, input);
    if (!valid) {
        if (execute_failures_metric_) {
            execute_failures_metric_->increment();
        }
        if (execute_duration_metric_) {
            execute_duration_metric_->record(elapsed_seconds(started));
        }
        return Result<nlohmann::json>::failure(std::move(valid).error());
    }
    auto result = manager_->execute_validated(operation, input);
    if (result) {
        if (execute_success_metric_) {
            execute_success_metric_->increment();
        }
    } else if (execute_failures_metric_) {
        execute_failures_metric_->increment();
    }
    if (execute_duration_metric_) {
        execute_duration_metric_->record(elapsed_seconds(started));
    }
    return result;
}

Result<void> PluginRuntime::validate_for_execution(
    const std::string_view operation,
    const nlohmann::json& input) const {
    if (validate_calls_metric_) {
        validate_calls_metric_->increment();
    }
    const auto started = std::chrono::steady_clock::now();
    auto lease = acquire_execution_lease(operation);
    if (!lease) {
        if (validate_failures_metric_) {
            validate_failures_metric_->increment();
        }
        if (validate_duration_metric_) {
            validate_duration_metric_->record(elapsed_seconds(started));
        }
        return Result<void>::failure(std::move(lease).error());
    }
    auto result = manager_->validate_plugin_contract(operation, input);
    if (!result && validate_failures_metric_) {
        validate_failures_metric_->increment();
    }
    if (validate_duration_metric_) {
        validate_duration_metric_->record(elapsed_seconds(started));
    }
    return result;
}

Result<nlohmann::json> PluginRuntime::execute_validated(
    const std::string_view operation,
    const nlohmann::json& input) const {
    if (execute_calls_metric_) {
        execute_calls_metric_->increment();
    }
    const auto started = std::chrono::steady_clock::now();
    auto lease = acquire_execution_lease(operation);
    if (!lease) {
        if (execute_failures_metric_) {
            execute_failures_metric_->increment();
        }
        if (execute_duration_metric_) {
            execute_duration_metric_->record(elapsed_seconds(started));
        }
        return Result<nlohmann::json>::failure(std::move(lease).error());
    }
    auto result = manager_->execute_validated(operation, input);
    if (result) {
        if (execute_success_metric_) {
            execute_success_metric_->increment();
        }
    } else if (execute_failures_metric_) {
        execute_failures_metric_->increment();
    }
    if (execute_duration_metric_) {
        execute_duration_metric_->record(elapsed_seconds(started));
    }
    return result;
}

Result<void> PluginRuntime::shutdown() {
    std::unique_lock<std::mutex> lock(lease_state_->mutex);
    if (lease_state_->state == PluginRuntimeState::Stopped) {
        if (lease_state_->shutdown_error.has_value()) {
            return Result<void>::failure(*lease_state_->shutdown_error);
        }
        return Result<void>::success();
    }
    if (lease_state_->shutdown_in_progress) {
        lease_state_->changed.wait(lock, [this] {
            return lease_state_->state == PluginRuntimeState::Stopped;
        });
        if (lease_state_->shutdown_error.has_value()) {
            return Result<void>::failure(*lease_state_->shutdown_error);
        }
        return Result<void>::success();
    }

    if (lease_state_->registrations_in_progress != 0U) {
        lease_state_->changed.wait(lock, [this] {
            return lease_state_->registrations_in_progress == 0U;
        });
    }

    if (shutdown_metric_) {
        shutdown_metric_->increment();
    }
    set_state_locked(PluginRuntimeState::Draining);
    for (const auto& item : entries_) {
        if (item.second && item.second->state == PluginEntryState::Ready) {
            set_entry_state_locked(item.second, PluginEntryState::Draining);
        }
    }
    lease_state_->shutdown_in_progress = true;
    lease_state_->changed.wait(lock, [this] {
        return lease_state_->active_executions == 0U;
    });
    lock.unlock();

    std::optional<Error> first_error;
    for (auto item = managed_plugins_.rbegin();
         item != managed_plugins_.rend();
         ++item) {
        const auto& entry = item->entry;
        if (!entry || !entry->lifecycle) {
            continue;
        }
        bool entry_shutdown_failed = false;
        try {
            auto result = entry->lifecycle->shutdown();
            if (!result) {
                entry_shutdown_failed = true;
                if (!first_error.has_value()) {
                    first_error = fixed_error(
                        result.error().code,
                        result.error().message,
                        limits_.max_error_message_bytes());
                }
            }
        } catch (const std::exception&) {
            entry_shutdown_failed = true;
            if (!first_error.has_value()) {
                first_error = fixed_internal_error("plugin shutdown failed");
            }
        } catch (...) {
            entry_shutdown_failed = true;
            if (!first_error.has_value()) {
                first_error = fixed_internal_error("plugin shutdown failed");
            }
        }
        lock.lock();
        entry->shutdown_failed = entry_shutdown_failed;
        entry->lifecycle.reset();
        set_entry_state_locked(entry, PluginEntryState::Stopped);
        lock.unlock();
    }
    managed_plugins_.clear();

    lock.lock();
    for (const auto& item : entries_) {
        if (item.second && item.second->state == PluginEntryState::Draining) {
            set_entry_state_locked(item.second, PluginEntryState::Stopped);
        }
    }
    lease_state_->shutdown_error = first_error;
    set_state_locked(PluginRuntimeState::Stopped);
    lease_state_->shutdown_in_progress = false;
    lock.unlock();
    lease_state_->changed.notify_all();

    if (first_error.has_value()) {
        if (shutdown_failures_metric_) {
            shutdown_failures_metric_->increment();
        }
        return Result<void>::failure(*first_error);
    }
    return Result<void>::success();
}

Error PluginRuntime::fixed_internal_error(
    const std::string_view message) const {
    return fixed_error(
        ErrorCode::InternalError,
        message,
        limits_.max_error_message_bytes());
}

}  // namespace iaisf::plugin
