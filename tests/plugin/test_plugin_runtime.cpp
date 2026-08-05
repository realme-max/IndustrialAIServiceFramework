#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/plugin_runtime.hpp"
#include "iaisf/plugin/plugin_task_adapter.hpp"

namespace {

using iaisf::ErrorCode;
using iaisf::MetricsRegistry;
using iaisf::Result;
using iaisf::make_error;
using iaisf::plugin::IAlgorithmPlugin;
using iaisf::plugin::IManagedAlgorithmPlugin;
using iaisf::plugin::PluginEntryState;
using iaisf::plugin::PluginEntrySnapshot;
using iaisf::plugin::PluginLimits;
using iaisf::plugin::PluginMetadata;
using iaisf::plugin::PluginRuntime;
using iaisf::plugin::PluginRuntimeState;
using iaisf::plugin::PluginTaskAdapter;

class ManagedPlugin final : public IAlgorithmPlugin,
                            public IManagedAlgorithmPlugin {
public:
    ManagedPlugin(
        std::string operation,
        std::shared_ptr<std::vector<std::string>> lifecycle,
        std::shared_ptr<std::mutex> lifecycle_mutex)
        : metadata_{
              std::move(operation),
              "Managed Test Plugin",
              "1.0.0",
              "Plugin runtime lifecycle test plugin.",
              false,
              {}},
          lifecycle_(std::move(lifecycle)),
          lifecycle_mutex_(std::move(lifecycle_mutex)) {}

    PluginMetadata metadata() const override {
        return metadata_;
    }

    Result<void> validate_input(const nlohmann::json&) const override {
        return Result<void>::success();
    }

    Result<nlohmann::json> execute(const nlohmann::json& input) const override {
        std::unique_lock<std::mutex> lock(execute_mutex_);
        if (block_execute_) {
            execute_entered_ = true;
            execute_changed_.notify_all();
            execute_changed_.wait(lock, [this] { return allow_execute_; });
        }
        return Result<nlohmann::json>::success(input);
    }

    Result<void> initialize() override {
        {
            std::unique_lock<std::mutex> lock(initialize_mutex_);
            if (block_initialize_) {
                initialize_entered_ = true;
                initialize_changed_.notify_all();
                initialize_changed_.wait(
                    lock, [this] { return allow_initialize_; });
            }
        }
        initialize_calls_.fetch_add(1U, std::memory_order_relaxed);
        record("initialize:" + metadata_.operation);
        if (throw_initialize_) {
            throw std::runtime_error{"private initialization detail"};
        }
        if (throw_unknown_initialize_) {
            throw 7;
        }
        if (fail_initialize_) {
            return Result<void>::failure(make_error(
                ErrorCode::ConfigError,
                "managed plugin initialization rejected"));
        }
        return Result<void>::success();
    }

    Result<void> shutdown() override {
        shutdown_calls_.fetch_add(1U, std::memory_order_relaxed);
        record("shutdown:" + metadata_.operation);
        if (throw_shutdown_) {
            throw std::runtime_error{"private shutdown detail"};
        }
        if (throw_unknown_shutdown_) {
            throw 9;
        }
        if (fail_shutdown_) {
            return Result<void>::failure(make_error(
                ErrorCode::InternalError,
                "managed plugin shutdown rejected"));
        }
        return Result<void>::success();
    }

    void set_block_execute(const bool value) {
        std::lock_guard<std::mutex> lock(execute_mutex_);
        block_execute_ = value;
        if (!value) {
            allow_execute_ = true;
            execute_changed_.notify_all();
        }
    }

    void set_block_initialize(const bool value) {
        std::lock_guard<std::mutex> lock(initialize_mutex_);
        block_initialize_ = value;
        if (!value) {
            allow_initialize_ = true;
            initialize_changed_.notify_all();
        }
    }

    void release_initialize() {
        std::lock_guard<std::mutex> lock(initialize_mutex_);
        allow_initialize_ = true;
        initialize_changed_.notify_all();
    }

    void wait_until_initialize_entered() {
        std::unique_lock<std::mutex> lock(initialize_mutex_);
        initialize_changed_.wait(lock, [this] { return initialize_entered_; });
    }

    void release_execute() {
        std::lock_guard<std::mutex> lock(execute_mutex_);
        allow_execute_ = true;
        execute_changed_.notify_all();
    }

    void wait_until_execute_entered() {
        std::unique_lock<std::mutex> lock(execute_mutex_);
        execute_changed_.wait(lock, [this] { return execute_entered_; });
    }

    std::atomic<std::size_t> initialize_calls_{0U};
    std::atomic<std::size_t> shutdown_calls_{0U};
    bool fail_initialize_{false};
    bool throw_initialize_{false};
    bool throw_unknown_initialize_{false};
    bool fail_shutdown_{false};
    bool throw_shutdown_{false};
    bool throw_unknown_shutdown_{false};

private:
    void record(std::string entry) {
        std::lock_guard<std::mutex> lock(*lifecycle_mutex_);
        lifecycle_->push_back(std::move(entry));
    }

    PluginMetadata metadata_;
    std::shared_ptr<std::vector<std::string>> lifecycle_;
    std::shared_ptr<std::mutex> lifecycle_mutex_;
    mutable std::mutex execute_mutex_;
    mutable std::condition_variable execute_changed_;
    mutable bool block_execute_{false};
    mutable bool execute_entered_{false};
    mutable bool allow_execute_{false};
    mutable std::mutex initialize_mutex_;
    mutable std::condition_variable initialize_changed_;
    mutable bool block_initialize_{false};
    mutable bool initialize_entered_{false};
    mutable bool allow_initialize_{false};
};

struct LifecycleFixture {
    std::shared_ptr<std::vector<std::string>> events =
        std::make_shared<std::vector<std::string>>();
    std::shared_ptr<std::mutex> events_mutex = std::make_shared<std::mutex>();
};

std::shared_ptr<PluginRuntime> runtime_for(
    MetricsRegistry* const metrics = nullptr) {
    auto limits = PluginLimits::create();
    EXPECT_TRUE(limits);
    auto runtime = PluginRuntime::create(
        std::move(limits).value(), metrics);
    EXPECT_TRUE(runtime);
    return runtime ? std::move(runtime).value() : nullptr;
}

TEST(PluginRuntimeTest, StartsConfiguringAndSupportsStaticRegistration) {
    auto runtime = runtime_for();
    ASSERT_NE(runtime, nullptr);
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Configuring);
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<iaisf::plugin::EchoPlugin>()));
    EXPECT_EQ(runtime->size(), 1U);
    EXPECT_EQ(
        runtime->validate("echo", nlohmann::json{{"payload", 1}}).error().code,
        ErrorCode::InvalidState);
}

TEST(PluginRuntimeTest, FreezeIsIdempotentAndRejectsRegistration) {
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->freeze());
    ASSERT_TRUE(runtime->freeze());
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Frozen);
    const auto registered = runtime->register_plugin(
        std::make_shared<iaisf::plugin::EchoPlugin>());
    ASSERT_FALSE(registered);
    EXPECT_EQ(registered.error().code, ErrorCode::InvalidState);
}

TEST(PluginRuntimeTest, FreezeEnablesValidationAndExecution) {
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<iaisf::plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());
    ASSERT_TRUE(runtime->validate("echo", nlohmann::json{{"payload", 7}}));
    const auto result = runtime->execute(
        "echo", nlohmann::json{{"payload", 7}});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 7);
}

TEST(PluginRuntimeTest, ManagedPluginsInitializeAndShutdownInReverseOrder) {
    LifecycleFixture fixture;
    auto first = std::make_shared<ManagedPlugin>(
        "first", fixture.events, fixture.events_mutex);
    auto second = std::make_shared<ManagedPlugin>(
        "second", fixture.events, fixture.events_mutex);
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(first));
    ASSERT_TRUE(runtime->register_plugin(second));
    ASSERT_TRUE(runtime->freeze());
    ASSERT_TRUE(runtime->shutdown());
    ASSERT_EQ(first->initialize_calls_.load(), 1U);
    ASSERT_EQ(second->initialize_calls_.load(), 1U);
    ASSERT_EQ(first->shutdown_calls_.load(), 1U);
    ASSERT_EQ(second->shutdown_calls_.load(), 1U);
    ASSERT_GE(fixture.events->size(), 4U);
    EXPECT_EQ(fixture.events->at(fixture.events->size() - 2U),
              "shutdown:second");
    EXPECT_EQ(fixture.events->back(), "shutdown:first");
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Stopped);
}

TEST(PluginRuntimeTest, InitializationFailureIsSanitized) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "failing", fixture.events, fixture.events_mutex);
    plugin->throw_initialize_ = true;
    auto runtime = runtime_for();
    const auto result = runtime->register_plugin(plugin);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InternalError);
    EXPECT_EQ(result.error().message, "plugin initialization failed");
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Configuring);
    EXPECT_EQ(plugin->shutdown_calls_.load(), 1U);
}

TEST(PluginRuntimeTest, PreviousManagedPluginsRemainRollbackSafeAfterFailure) {
    LifecycleFixture fixture;
    auto first = std::make_shared<ManagedPlugin>(
        "first", fixture.events, fixture.events_mutex);
    auto failing = std::make_shared<ManagedPlugin>(
        "failing", fixture.events, fixture.events_mutex);
    failing->fail_initialize_ = true;
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(first));
    ASSERT_FALSE(runtime->register_plugin(failing));
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Configuring);
    ASSERT_TRUE(runtime->shutdown());
    EXPECT_EQ(first->shutdown_calls_.load(), 1U);
    EXPECT_EQ(failing->shutdown_calls_.load(), 1U);
    EXPECT_EQ(fixture.events->back(), "shutdown:first");
}

TEST(PluginRuntimeTest, UnknownInitializationExceptionIsSanitized) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "unknown", fixture.events, fixture.events_mutex);
    plugin->throw_unknown_initialize_ = true;
    auto runtime = runtime_for();
    const auto result = runtime->register_plugin(plugin);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InternalError);
    EXPECT_EQ(result.error().message, "plugin initialization failed");
    EXPECT_EQ(plugin->shutdown_calls_.load(), 1U);
}

TEST(PluginRuntimeTest, DuplicateRegistrationRollsBackManagedPlugin) {
    LifecycleFixture fixture;
    auto first = std::make_shared<ManagedPlugin>(
        "same", fixture.events, fixture.events_mutex);
    auto second = std::make_shared<ManagedPlugin>(
        "same", fixture.events, fixture.events_mutex);
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(first));
    const auto duplicate = runtime->register_plugin(second);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(second->initialize_calls_.load(), 0U);
    EXPECT_EQ(second->shutdown_calls_.load(), 0U);
    ASSERT_TRUE(runtime->freeze());
    EXPECT_TRUE(runtime->execute("same", {}).has_value());
}

TEST(PluginRuntimeTest, LeaseKeepsPluginAliveUntilExecutionCompletes) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "blocking", fixture.events, fixture.events_mutex);
    plugin->set_block_execute(true);
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(plugin));
    ASSERT_TRUE(runtime->freeze());

    auto execute = std::async(std::launch::async, [&runtime] {
        return runtime->execute(
            "blocking", nlohmann::json{{"value", 1}});
    });
    plugin->wait_until_execute_entered();
    auto shutdown = std::async(std::launch::async, [&runtime] {
        return runtime->shutdown();
    });
    for (std::size_t attempt = 0U;
         attempt < 10000U && runtime->state() != PluginRuntimeState::Draining;
         ++attempt) {
        std::this_thread::yield();
    }
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Draining);
    EXPECT_EQ(runtime->active_execution_count(), 1U);
    EXPECT_EQ(plugin->shutdown_calls_.load(), 0U);
    plugin->release_execute();
    ASSERT_TRUE(execute.get());
    ASSERT_TRUE(shutdown.get());
    EXPECT_EQ(plugin->shutdown_calls_.load(), 1U);
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Stopped);
}

TEST(PluginRuntimeTest, ConcurrentShutdownIsIdempotent) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "shutdown", fixture.events, fixture.events_mutex);
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(plugin));
    ASSERT_TRUE(runtime->freeze());
    auto first = std::async(std::launch::async, [&runtime] {
        return runtime->shutdown();
    });
    auto second = std::async(std::launch::async, [&runtime] {
        return runtime->shutdown();
    });
    ASSERT_TRUE(first.get());
    ASSERT_TRUE(second.get());
    ASSERT_TRUE(runtime->shutdown());
    EXPECT_EQ(plugin->shutdown_calls_.load(), 1U);
}

TEST(PluginRuntimeTest, DrainingRejectsNewExecution) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "drain", fixture.events, fixture.events_mutex);
    plugin->set_block_execute(true);
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(plugin));
    ASSERT_TRUE(runtime->freeze());
    auto execute = std::async(std::launch::async, [&runtime] {
        return runtime->execute("drain", {});
    });
    plugin->wait_until_execute_entered();
    auto shutdown = std::async(std::launch::async, [&runtime] {
        return runtime->shutdown();
    });
    for (std::size_t attempt = 0U;
         attempt < 10000U && runtime->state() != PluginRuntimeState::Draining;
         ++attempt) {
        std::this_thread::yield();
    }
    const auto rejected = runtime->validate("drain", {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidState);
    plugin->release_execute();
    ASSERT_TRUE(execute.get());
    ASSERT_TRUE(shutdown.get());
}

TEST(PluginRuntimeTest, ShutdownFailureDoesNotSkipOtherPlugins) {
    LifecycleFixture fixture;
    auto first = std::make_shared<ManagedPlugin>(
        "first", fixture.events, fixture.events_mutex);
    auto second = std::make_shared<ManagedPlugin>(
        "second", fixture.events, fixture.events_mutex);
    second->fail_shutdown_ = true;
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(first));
    ASSERT_TRUE(runtime->register_plugin(second));
    ASSERT_TRUE(runtime->freeze());
    const auto stopped = runtime->shutdown();
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::InternalError);
    EXPECT_EQ(first->shutdown_calls_.load(), 1U);
    EXPECT_EQ(second->shutdown_calls_.load(), 1U);
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Stopped);
    const auto repeated = runtime->shutdown();
    ASSERT_FALSE(repeated);
    EXPECT_EQ(repeated.error().code, ErrorCode::InternalError);
}

TEST(PluginRuntimeTest, UnknownShutdownExceptionDoesNotSkipOtherPlugins) {
    LifecycleFixture fixture;
    auto first = std::make_shared<ManagedPlugin>(
        "first", fixture.events, fixture.events_mutex);
    auto second = std::make_shared<ManagedPlugin>(
        "second", fixture.events, fixture.events_mutex);
    second->throw_unknown_shutdown_ = true;
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(first));
    ASSERT_TRUE(runtime->register_plugin(second));
    ASSERT_TRUE(runtime->freeze());
    const auto stopped = runtime->shutdown();
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::InternalError);
    EXPECT_EQ(first->shutdown_calls_.load(), 1U);
    EXPECT_EQ(second->shutdown_calls_.load(), 1U);
}

TEST(PluginRuntimeTest, StaticPluginsRemainConcurrentAndIndependent) {
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<iaisf::plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());
    auto first = std::async(std::launch::async, [&runtime] {
        return runtime->execute(
            "echo", nlohmann::json{{"payload", 1}});
    });
    auto second = std::async(std::launch::async, [&runtime] {
        return runtime->execute(
            "echo", nlohmann::json{{"payload", 2}});
    });
    ASSERT_TRUE(first.get());
    ASSERT_TRUE(second.get());
}

TEST(PluginRuntimeTest, ShutdownBeforeFreezeIsSafeAndTerminal) {
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->shutdown());
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Stopped);
    EXPECT_FALSE(runtime->freeze());
    EXPECT_FALSE(runtime->validate("missing", {}));
    EXPECT_FALSE(runtime->execute("missing", {}));
}

TEST(PluginRuntimeTest, TaskAdapterUsesRuntimeAndPreservesSecondValidation) {
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<iaisf::plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());
    auto adapter = PluginTaskAdapter::create(runtime);
    ASSERT_TRUE(adapter);
    iaisf::task::TaskRequest request{
        "echo", nlohmann::json{{"payload", "runtime"}}};
    ASSERT_TRUE(adapter.value()->validate_task(request));
    const auto result = adapter.value()->execute_task(request);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), "runtime");
}

TEST(PluginRuntimeTest, MetricsTrackLifecycleCallsAndState) {
    MetricsRegistry metrics;
    auto runtime = runtime_for(&metrics);
    ASSERT_NE(runtime, nullptr);

    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<iaisf::plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());
    ASSERT_TRUE(runtime->validate("echo", nlohmann::json{{"payload", 1}}));
    ASSERT_TRUE(runtime->execute(
        "echo", nlohmann::json{{"payload", 2}}));
    EXPECT_FALSE(runtime->validate("missing", nlohmann::json::object()));
    ASSERT_TRUE(runtime->shutdown());

    const auto registrations = metrics.get_counter(
        "plugin_runtime_registrations_total");
    const auto registration_failures = metrics.get_counter(
        "plugin_runtime_registration_failures_total");
    const auto initializations = metrics.get_counter(
        "plugin_runtime_initializations_total");
    const auto initialization_failures = metrics.get_counter(
        "plugin_runtime_initialization_failures_total");
    const auto validate_calls = metrics.get_counter(
        "plugin_runtime_validate_calls_total");
    const auto validate_failures = metrics.get_counter(
        "plugin_runtime_validate_failures_total");
    const auto execute_calls = metrics.get_counter(
        "plugin_runtime_execute_calls_total");
    const auto execute_success = metrics.get_counter(
        "plugin_runtime_execute_success_total");
    const auto execute_failures = metrics.get_counter(
        "plugin_runtime_execute_failures_total");
    const auto shutdowns = metrics.get_counter("plugin_runtime_shutdown_total");
    const auto shutdown_failures = metrics.get_counter(
        "plugin_runtime_shutdown_failures_total");
    const auto registered = metrics.get_gauge("plugin_runtime_registered");
    const auto active = metrics.get_gauge("plugin_runtime_active_executions");
    const auto state = metrics.get_gauge("plugin_runtime_state");
    const auto validate_duration = metrics.get_histogram(
        "plugin_validate_duration_seconds");
    const auto execute_duration = metrics.get_histogram(
        "plugin_execute_duration_seconds");

    ASSERT_TRUE(registrations);
    ASSERT_TRUE(registration_failures);
    ASSERT_TRUE(initializations);
    ASSERT_TRUE(initialization_failures);
    ASSERT_TRUE(validate_calls);
    ASSERT_TRUE(validate_failures);
    ASSERT_TRUE(execute_calls);
    ASSERT_TRUE(execute_success);
    ASSERT_TRUE(execute_failures);
    ASSERT_TRUE(shutdowns);
    ASSERT_TRUE(shutdown_failures);
    ASSERT_TRUE(registered);
    ASSERT_TRUE(active);
    ASSERT_TRUE(state);
    ASSERT_TRUE(validate_duration);
    ASSERT_TRUE(execute_duration);

    EXPECT_EQ(registrations.value()->snapshot(), 1U);
    EXPECT_EQ(registration_failures.value()->snapshot(), 0U);
    EXPECT_EQ(initializations.value()->snapshot(), 1U);
    EXPECT_EQ(initialization_failures.value()->snapshot(), 0U);
    EXPECT_EQ(validate_calls.value()->snapshot(), 2U);
    EXPECT_EQ(validate_failures.value()->snapshot(), 1U);
    EXPECT_EQ(execute_calls.value()->snapshot(), 1U);
    EXPECT_EQ(execute_success.value()->snapshot(), 1U);
    EXPECT_EQ(execute_failures.value()->snapshot(), 0U);
    EXPECT_EQ(shutdowns.value()->snapshot(), 1U);
    EXPECT_EQ(shutdown_failures.value()->snapshot(), 0U);
    EXPECT_EQ(registered.value()->snapshot(), 1);
    EXPECT_EQ(active.value()->snapshot(), 0);
    EXPECT_EQ(state.value()->snapshot(), 3);
    EXPECT_EQ(validate_duration.value()->snapshot().count, 2U);
    EXPECT_EQ(execute_duration.value()->snapshot().count, 1U);
}

TEST(PluginRuntimeTest, MetricsTrackActiveLeaseAndDrainingRejection) {
    MetricsRegistry metrics;
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "blocking.metrics", fixture.events, fixture.events_mutex);
    plugin->set_block_execute(true);
    auto runtime = runtime_for(&metrics);
    ASSERT_TRUE(runtime->register_plugin(plugin));
    ASSERT_TRUE(runtime->freeze());

    auto execution = std::async(std::launch::async, [&runtime] {
        return runtime->execute("blocking.metrics", nlohmann::json::object());
    });
    plugin->wait_until_execute_entered();
    auto shutdown = std::async(std::launch::async, [&runtime] {
        return runtime->shutdown();
    });
    for (std::size_t attempt = 0U;
         attempt < 10000U && runtime->state() != PluginRuntimeState::Draining;
         ++attempt) {
        std::this_thread::yield();
    }

    EXPECT_EQ(runtime->active_execution_count(), 1U);
    auto active = metrics.get_gauge("plugin_runtime_active_executions");
    ASSERT_TRUE(active);
    EXPECT_EQ(active.value()->snapshot(), 1);
    const auto rejected = runtime->validate(
        "blocking.metrics", nlohmann::json::object());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidState);

    plugin->release_execute();
    ASSERT_TRUE(execution.get());
    ASSERT_TRUE(shutdown.get());
    EXPECT_EQ(runtime->active_execution_count(), 0U);
    EXPECT_EQ(active.value()->snapshot(), 0);

    auto validate_calls = metrics.get_counter(
        "plugin_runtime_validate_calls_total");
    auto validate_failures = metrics.get_counter(
        "plugin_runtime_validate_failures_total");
    ASSERT_TRUE(validate_calls);
    ASSERT_TRUE(validate_failures);
    EXPECT_EQ(validate_calls.value()->snapshot(), 1U);
    EXPECT_EQ(validate_failures.value()->snapshot(), 1U);
}

TEST(PluginRuntimeTest, MetricsFailureDoesNotAffectPluginExecution) {
    MetricsRegistry metrics;
    ASSERT_TRUE(metrics.create_gauge(
        "plugin_runtime_execute_calls_total"));
    auto runtime = runtime_for(&metrics);
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<iaisf::plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());

    const auto result = runtime->execute(
        "echo", nlohmann::json{{"payload", "metrics"}});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), "metrics");
    EXPECT_EQ(metrics.size(), 16U);
}

TEST(PluginRuntimeTest, EntryTransitionsThroughInitializingAndReady) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "entry.lifecycle", fixture.events, fixture.events_mutex);
    plugin->set_block_initialize(true);
    auto runtime = runtime_for();

    auto registration = std::async(
        std::launch::async, [&runtime, plugin] {
            return runtime->register_plugin(plugin);
        });
    plugin->wait_until_initialize_entered();

    const auto initializing = runtime->entry_snapshot("entry.lifecycle");
    ASSERT_TRUE(initializing);
    EXPECT_EQ(initializing.value().operation, "entry.lifecycle");
    EXPECT_EQ(initializing.value().metadata.operation, "entry.lifecycle");
    EXPECT_TRUE(initializing.value().managed_lifecycle);
    EXPECT_EQ(initializing.value().state, PluginEntryState::Initializing);
    EXPECT_EQ(initializing.value().active_execution_count, 0U);
    EXPECT_FALSE(initializing.value().shutdown_failed);

    plugin->release_initialize();
    ASSERT_TRUE(registration.get());
    const auto ready = runtime->entry_snapshot("entry.lifecycle");
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready.value().state, PluginEntryState::Ready);
}

TEST(PluginRuntimeTest, FailedInitializationLeavesFailedEntryObservation) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "entry.failed", fixture.events, fixture.events_mutex);
    plugin->fail_initialize_ = true;
    auto runtime = runtime_for();

    const auto registration = runtime->register_plugin(plugin);
    ASSERT_FALSE(registration);
    const auto failed = runtime->entry_snapshot("entry.failed");
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed.value().state, PluginEntryState::Failed);
    EXPECT_TRUE(failed.value().managed_lifecycle);
    EXPECT_EQ(failed.value().active_execution_count, 0U);
    EXPECT_FALSE(failed.value().shutdown_failed);

    plugin->fail_initialize_ = false;
    ASSERT_TRUE(runtime->register_plugin(plugin));
    const auto recovered = runtime->entry_snapshot("entry.failed");
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered.value().state, PluginEntryState::Ready);
}

TEST(PluginRuntimeTest, EntryActiveCountsRemainIndependent) {
    LifecycleFixture fixture;
    auto first = std::make_shared<ManagedPlugin>(
        "entry.first", fixture.events, fixture.events_mutex);
    auto second = std::make_shared<ManagedPlugin>(
        "entry.second", fixture.events, fixture.events_mutex);
    first->set_block_execute(true);
    second->set_block_execute(true);
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(first));
    ASSERT_TRUE(runtime->register_plugin(second));
    ASSERT_TRUE(runtime->freeze());

    auto first_execution = std::async(
        std::launch::async, [&runtime] {
            return runtime->execute("entry.first", nlohmann::json{{"id", 1}});
        });
    auto second_execution = std::async(
        std::launch::async, [&runtime] {
            return runtime->execute(
                "entry.second", nlohmann::json{{"id", 2}});
        });
    first->wait_until_execute_entered();
    second->wait_until_execute_entered();

    const auto snapshots = runtime->entry_snapshots();
    ASSERT_TRUE(snapshots);
    ASSERT_EQ(snapshots.value().size(), 2U);
    EXPECT_EQ(snapshots.value().at(0).operation, "entry.first");
    EXPECT_EQ(snapshots.value().at(0).active_execution_count, 1U);
    EXPECT_EQ(snapshots.value().at(1).operation, "entry.second");
    EXPECT_EQ(snapshots.value().at(1).active_execution_count, 1U);
    EXPECT_EQ(runtime->active_execution_count(), 2U);

    first->release_execute();
    second->release_execute();
    ASSERT_TRUE(first_execution.get());
    ASSERT_TRUE(second_execution.get());
    const auto completed = runtime->entry_snapshots();
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed.value().at(0).active_execution_count, 0U);
    EXPECT_EQ(completed.value().at(1).active_execution_count, 0U);
}

TEST(PluginRuntimeTest, EntryMovesToDrainingThenStoppedAfterLeaseRelease) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "entry.draining", fixture.events, fixture.events_mutex);
    plugin->set_block_execute(true);
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(plugin));
    ASSERT_TRUE(runtime->freeze());

    auto execution = std::async(
        std::launch::async, [&runtime] {
            return runtime->execute(
                "entry.draining", nlohmann::json{{"payload", true}});
        });
    plugin->wait_until_execute_entered();
    auto shutdown = std::async(
        std::launch::async, [&runtime] { return runtime->shutdown(); });
    for (std::size_t attempt = 0U;
         attempt < 10000U && runtime->state() != PluginRuntimeState::Draining;
         ++attempt) {
        std::this_thread::yield();
    }
    ASSERT_EQ(runtime->state(), PluginRuntimeState::Draining);

    const auto draining = runtime->entry_snapshot("entry.draining");
    ASSERT_TRUE(draining);
    EXPECT_EQ(draining.value().state, PluginEntryState::Draining);
    EXPECT_EQ(draining.value().active_execution_count, 1U);

    plugin->release_execute();
    ASSERT_TRUE(execution.get());
    ASSERT_TRUE(shutdown.get());

    const auto stopped = runtime->entry_snapshot("entry.draining");
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped.value().state, PluginEntryState::Stopped);
    EXPECT_EQ(stopped.value().active_execution_count, 0U);
    EXPECT_FALSE(stopped.value().shutdown_failed);
    ASSERT_TRUE(runtime->shutdown());
}

TEST(PluginRuntimeTest, EntryRecordsShutdownFailureWithoutRuntimeCorruption) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "entry.shutdown_failure", fixture.events, fixture.events_mutex);
    plugin->fail_shutdown_ = true;
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(plugin));
    ASSERT_TRUE(runtime->freeze());

    const auto shutdown = runtime->shutdown();
    ASSERT_FALSE(shutdown);
    EXPECT_EQ(runtime->state(), PluginRuntimeState::Stopped);
    const auto snapshot = runtime->entry_snapshot("entry.shutdown_failure");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, PluginEntryState::Stopped);
    EXPECT_TRUE(snapshot.value().shutdown_failed);
    const auto repeated = runtime->shutdown();
    ASSERT_FALSE(repeated);
    EXPECT_EQ(repeated.error().code, ErrorCode::InternalError);
}

TEST(PluginRuntimeTest, EntrySnapshotsCanRunConcurrentlyWithExecution) {
    LifecycleFixture fixture;
    auto plugin = std::make_shared<ManagedPlugin>(
        "entry.snapshot", fixture.events, fixture.events_mutex);
    plugin->set_block_execute(true);
    auto runtime = runtime_for();
    ASSERT_TRUE(runtime->register_plugin(plugin));
    ASSERT_TRUE(runtime->freeze());

    auto execution = std::async(
        std::launch::async, [&runtime] {
            return runtime->execute(
                "entry.snapshot", nlohmann::json{{"value", 1}});
        });
    plugin->wait_until_execute_entered();
    auto snapshots = std::async(
        std::launch::async, [&runtime] {
            for (std::size_t index = 0U; index < 128U; ++index) {
                auto result = runtime->entry_snapshots();
                if (!result || result.value().size() != 1U) {
                    return false;
                }
            }
            return true;
        });
    ASSERT_TRUE(snapshots.get());
    plugin->release_execute();
    ASSERT_TRUE(execution.get());
}

}  // namespace
