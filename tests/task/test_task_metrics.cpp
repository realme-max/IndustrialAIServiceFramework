#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/logging/logger.hpp"
#include "iaisf/metrics/metrics.hpp"
#include "iaisf/task/task_manager.hpp"

namespace {

class QuietLogger final : public iaisf::ILogger {
public:
    void log(iaisf::LogLevel, std::string_view, std::string_view) override {}
};

TEST(TaskMetricsTest, TracksSubmitRunningSuccessAndFailure) {
    QuietLogger logger;
    iaisf::MetricsRegistry metrics;
    std::mutex observation_mutex;
    std::condition_variable observation_changed;
    int handler_calls = 0;
    std::int64_t maximum_running = 0;

    auto manager_result = iaisf::task::TaskManager::create(
        iaisf::task::ThreadPoolOptions{2U, 8U},
        iaisf::task::TaskLimits::create().value(),
        logger,
        [&metrics, &observation_mutex, &observation_changed, &handler_calls,
         &maximum_running](const iaisf::task::TaskRequest& request) {
            const auto running = metrics.get_gauge("tasks_running");
            {
                std::lock_guard<std::mutex> lock{observation_mutex};
                maximum_running = std::max(
                    maximum_running,
                    running ? running.value()->snapshot() : -1);
                ++handler_calls;
            }
            observation_changed.notify_all();
            if (request.operation == "success") {
                return iaisf::Result<nlohmann::json>::success(
                    nlohmann::json{{"ok", true}});
            }
            return iaisf::Result<nlohmann::json>::failure(
                iaisf::make_error(
                    iaisf::ErrorCode::InternalError,
                    "expected task failure"));
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();

    auto success = manager->submit(iaisf::task::TaskRequest{"success", {}});
    auto failure = manager->submit(iaisf::task::TaskRequest{"failure", {}});
    ASSERT_TRUE(success);
    ASSERT_TRUE(failure);

    {
        std::unique_lock<std::mutex> lock{observation_mutex};
        ASSERT_TRUE(observation_changed.wait_for(
            lock,
            std::chrono::seconds(5),
            [&handler_calls] { return handler_calls == 2; }));
    }
    ASSERT_TRUE(manager->shutdown());

    const auto submitted = metrics.get_counter("tasks_submitted_total");
    const auto completed = metrics.get_counter("tasks_completed_total");
    const auto failed = metrics.get_counter("tasks_failed_total");
    const auto running = metrics.get_gauge("tasks_running");
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(completed);
    ASSERT_TRUE(failed);
    ASSERT_TRUE(running);
    EXPECT_EQ(submitted.value()->snapshot(), 2U);
    EXPECT_EQ(completed.value()->snapshot(), 1U);
    EXPECT_EQ(failed.value()->snapshot(), 1U);
    EXPECT_EQ(running.value()->snapshot(), 0);
    {
        std::lock_guard<std::mutex> lock{observation_mutex};
        EXPECT_GE(maximum_running, 1);
    }

    ASSERT_TRUE(manager->get_snapshot(success.value()));
    ASSERT_TRUE(manager->get_snapshot(failure.value()));
    EXPECT_EQ(
        manager->get_snapshot(success.value()).value().state,
        iaisf::task::TaskState::Succeeded);
    EXPECT_EQ(
        manager->get_snapshot(failure.value()).value().state,
        iaisf::task::TaskState::Failed);
}

}  // namespace
