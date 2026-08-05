#include <gtest/gtest.h>

#include <string_view>
#include <future>

#include <nlohmann/json.hpp>

#include "iaisf/diagnostics/runtime_diagnostics.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/mock_vision_plugin.hpp"
#include "iaisf/plugin/plugin_runtime.hpp"

namespace iaisf::diagnostics {

namespace {
class QuietLogger final : public ILogger {
public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};

class StaticLogDiagnostics final : public ILogDiagnostics {
public:
    LogDiagnosticsSnapshot diagnostics_snapshot() const noexcept override {
        return LogDiagnosticsSnapshot{LogDiagnosticsState::Running, 7U, 2U,
                                      1U, 0U, 3U};
    }
};
}

TEST(RuntimeDiagnosticsTest, JsonContainsOnlyBoundedObservations) {
    RuntimeDiagnosticsSnapshot snapshot;
    snapshot.health = health::HealthStatus{health::HealthPhase::Running, true, true};
    snapshot.tasks.accepting = true;
    snapshot.tasks.stopped = false;
    snapshot.tasks.pending_count = 2U;
    snapshot.logger.available = false;
    snapshot.metrics.counters.push_back(CounterSnapshot{"tasks_submitted_total", 3U});

    auto encoded = to_json(snapshot, 4096U);
    ASSERT_TRUE(encoded);
    EXPECT_NE(encoded.value().find("tasks_submitted_total"), std::string::npos);
    EXPECT_NE(encoded.value().find("\"available\":false"), std::string::npos);
}

TEST(RuntimeDiagnosticsTest, ResponseLimitFailsClosed) {
    RuntimeDiagnosticsSnapshot snapshot;
    auto encoded = to_json(snapshot, 1U);
    EXPECT_FALSE(encoded);
    EXPECT_EQ(encoded.error().code, ErrorCode::ResourceExhausted);
}

TEST(RuntimeDiagnosticsTest, SnapshotReadsHealthTasksMetricsAndLogger) {
    QuietLogger logger;
    StaticLogDiagnostics log_diagnostics;
    MetricsRegistry metrics;
    auto health = std::make_shared<health::HealthChecker>();
    auto manager_result = task::TaskManager::create(
        task::ThreadPoolOptions{1U, 4U}, task::TaskLimits::create().value(),
        logger,
        [](const task::TaskRequest&) {
            return Result<nlohmann::json>::success(nlohmann::json::object());
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    auto diagnostics_result = RuntimeDiagnostics::create(
        health, *manager, metrics, &log_diagnostics);
    ASSERT_TRUE(diagnostics_result);
    auto diagnostics = std::move(diagnostics_result).value();

    for (const auto phase : {health::HealthPhase::Created,
                             health::HealthPhase::Running,
                             health::HealthPhase::Stopping}) {
        if (phase != health::HealthPhase::Created) {
            ASSERT_NE(health->transition_to(phase),
                      health::HealthTransitionOutcome::InvalidTransition);
        }
        auto snapshot = diagnostics->snapshot();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot.value().health.phase, phase);
    }
    EXPECT_TRUE(diagnostics->snapshot().value().logger.available);
    EXPECT_EQ(diagnostics->snapshot().value().logger.accepted, 7U);
    ASSERT_TRUE(manager->shutdown());
    ASSERT_NE(health->transition_to(health::HealthPhase::Stopped),
              health::HealthTransitionOutcome::InvalidTransition);
    EXPECT_EQ(diagnostics->snapshot().value().health.phase,
              health::HealthPhase::Stopped);
}

TEST(RuntimeDiagnosticsTest, SnapshotRaceWithTaskShutdownIsSafe) {
    QuietLogger logger;
    MetricsRegistry metrics;
    auto health = std::make_shared<health::HealthChecker>();
    auto manager_result = task::TaskManager::create(
        task::ThreadPoolOptions{1U, 4U}, task::TaskLimits::create().value(),
        logger,
        [](const task::TaskRequest&) {
            return Result<nlohmann::json>::success(nlohmann::json::object());
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    auto diagnostics_result = RuntimeDiagnostics::create(
        health, *manager, metrics);
    ASSERT_TRUE(diagnostics_result);
    auto diagnostics = std::move(diagnostics_result).value();
    auto snapshots = std::async(std::launch::async, [&diagnostics] {
        for (int index = 0; index < 256; ++index) {
            auto value = diagnostics->snapshot();
            if (!value) {
                return false;
            }
        }
        return true;
    });
    ASSERT_TRUE(manager->shutdown());
    EXPECT_TRUE(snapshots.get());
}

TEST(RuntimeDiagnosticsTest, PluginSnapshotIsSortedAndTracksActiveLease) {
    QuietLogger logger;
    MetricsRegistry metrics;
    auto health = std::make_shared<health::HealthChecker>();
    auto manager_result = task::TaskManager::create(
        task::ThreadPoolOptions{1U, 4U}, task::TaskLimits::create().value(),
        logger,
        [](const task::TaskRequest&) {
            return Result<nlohmann::json>::success(nlohmann::json::object());
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    auto limits = plugin::PluginLimits::create();
    ASSERT_TRUE(limits);
    auto runtime_result = plugin::PluginRuntime::create(
        std::move(limits).value(), &metrics);
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<plugin::MockVisionPlugin>()));
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());

    auto diagnostics_result = RuntimeDiagnostics::create(
        health, *manager, metrics, nullptr,
        std::weak_ptr<const plugin::PluginRuntime>{runtime});
    ASSERT_TRUE(diagnostics_result);
    auto diagnostics = std::move(diagnostics_result).value();
    {
        auto lease = runtime->acquire_execution_lease("echo");
        ASSERT_TRUE(lease);

        auto snapshot = diagnostics->snapshot();
        ASSERT_TRUE(snapshot);
        ASSERT_TRUE(snapshot.value().plugins.available);
        EXPECT_EQ(snapshot.value().plugins.registered_count, 2U);
        EXPECT_EQ(snapshot.value().plugins.active_executions, 1U);
        EXPECT_EQ(snapshot.value().plugins.managed_plugins, 2U);
        ASSERT_EQ(snapshot.value().plugins.entries.size(), 2U);
        EXPECT_EQ(snapshot.value().plugins.entries.at(0).operation, "echo");
        EXPECT_EQ(snapshot.value().plugins.entries.at(0).active_execution_count, 1U);
        EXPECT_EQ(snapshot.value().plugins.entries.at(1).operation,
                  "mock_vision.detect");
        EXPECT_EQ(snapshot.value().plugins.entries.at(1).active_execution_count, 0U);

        auto encoded = to_json(snapshot.value(), 8192U);
        ASSERT_TRUE(encoded);
        const auto entries_pos = encoded.value().find("\"entries\"");
        const auto echo_pos = encoded.value().find("echo", entries_pos);
        const auto mock_pos = encoded.value().find("mock_vision.detect", entries_pos);
        EXPECT_LT(entries_pos, encoded.value().size());
        EXPECT_LT(echo_pos, mock_pos);
    }
    ASSERT_TRUE(runtime->shutdown());
    ASSERT_TRUE(manager->shutdown());
}

TEST(RuntimeDiagnosticsTest, PluginSnapshotReportsShutdownState) {
    QuietLogger logger;
    MetricsRegistry metrics;
    auto health = std::make_shared<health::HealthChecker>();
    auto manager_result = task::TaskManager::create(
        task::ThreadPoolOptions{1U, 4U}, task::TaskLimits::create().value(),
        logger,
        [](const task::TaskRequest&) {
            return Result<nlohmann::json>::success(nlohmann::json::object());
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    auto runtime_result = plugin::PluginRuntime::create(
        plugin::PluginLimits::create().value(), &metrics);
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());
    auto diagnostics_result = RuntimeDiagnostics::create(
        health, *manager, metrics, nullptr,
        std::weak_ptr<const plugin::PluginRuntime>{runtime});
    ASSERT_TRUE(diagnostics_result);
    auto diagnostics = std::move(diagnostics_result).value();

    ASSERT_TRUE(runtime->shutdown());
    auto snapshot = diagnostics->snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().plugins.state,
              plugin::PluginRuntimeState::Stopped);
    ASSERT_EQ(snapshot.value().plugins.entries.size(), 1U);
    EXPECT_EQ(snapshot.value().plugins.entries.front().state,
              plugin::PluginEntryState::Stopped);
    EXPECT_FALSE(snapshot.value().plugins.shutdown_failed);
    ASSERT_TRUE(manager->shutdown());
}

TEST(RuntimeDiagnosticsTest, ExpiredPluginRuntimeIsUnavailable) {
    QuietLogger logger;
    MetricsRegistry metrics;
    auto health = std::make_shared<health::HealthChecker>();
    auto manager_result = task::TaskManager::create(
        task::ThreadPoolOptions{1U, 4U}, task::TaskLimits::create().value(),
        logger,
        [](const task::TaskRequest&) {
            return Result<nlohmann::json>::success(nlohmann::json::object());
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    auto runtime_result = plugin::PluginRuntime::create(
        plugin::PluginLimits::create().value(), &metrics);
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());
    auto diagnostics_result = RuntimeDiagnostics::create(
        health, *manager, metrics, nullptr,
        std::weak_ptr<const plugin::PluginRuntime>{runtime});
    ASSERT_TRUE(diagnostics_result);
    auto diagnostics = std::move(diagnostics_result).value();
    runtime.reset();

    auto snapshot = diagnostics->snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_FALSE(snapshot.value().plugins.available);
    EXPECT_TRUE(snapshot.value().plugins.entries.empty());
    ASSERT_TRUE(manager->shutdown());
}

} // namespace iaisf::diagnostics
