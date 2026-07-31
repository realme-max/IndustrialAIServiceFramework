#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/task/task_manager.hpp"

namespace iaisf::task {

class TaskManagerTestAccess {
public:
    [[nodiscard]] static Result<void> begin_submission(TaskManager& manager) {
        return manager.begin_submission();
    }

    static void finish_submission(TaskManager& manager) noexcept {
        manager.finish_submission();
    }

    static void wait_until_admission_closes(TaskManager& manager) {
        std::unique_lock<std::mutex> lock(manager.admission_mutex_);
        manager.submissions_finished_.wait(
            lock,
            [&manager] { return !manager.accepting_; });
    }
};

}  // namespace iaisf::task

namespace {

using namespace std::chrono_literals;
using iaisf::ErrorCode;
using iaisf::ILogger;
using iaisf::LogLevel;
using iaisf::Result;
using iaisf::make_error;
using iaisf::task::TaskLimits;
using iaisf::task::TaskManager;
using iaisf::task::TaskRequest;
using iaisf::task::TaskState;
using iaisf::task::ThreadPoolOptions;

class QuietLogger final : public ILogger {
public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};

std::unique_ptr<TaskManager> make_manager(
    QuietLogger& logger,
    iaisf::task::TaskHandler handler,
    const ThreadPoolOptions pool = ThreadPoolOptions{2, 16},
    TaskLimits limits = TaskLimits::create().value()) {
    auto created =
        TaskManager::create(pool, std::move(limits), logger, std::move(handler));
    EXPECT_TRUE(created);
    return created ? std::move(created).value() : nullptr;
}

std::unique_ptr<TaskManager> make_validating_manager(
    QuietLogger& logger,
    iaisf::task::TaskValidator validator,
    iaisf::task::TaskHandler handler,
    const ThreadPoolOptions pool = ThreadPoolOptions{2, 16},
    TaskLimits limits = TaskLimits::create().value()) {
    auto created = TaskManager::create(
        pool,
        std::move(limits),
        logger,
        std::move(validator),
        std::move(handler));
    EXPECT_TRUE(created);
    return created ? std::move(created).value() : nullptr;
}

TEST(TaskManagerTest, RejectsEmptyHandler) {
    QuietLogger logger;
    auto created = TaskManager::create(
        ThreadPoolOptions{1, 1},
        TaskLimits::create().value(),
        logger,
        {});

    EXPECT_FALSE(created);
    EXPECT_EQ(created.error().code, ErrorCode::InvalidArgument);
}

TEST(TaskManagerTest, InvalidRequestLeavesNoRepositoryRecord) {
    QuietLogger logger;
    auto manager = make_manager(
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);

    auto submitted = manager->submit(TaskRequest{"   ", {}});

    ASSERT_FALSE(submitted);
    EXPECT_EQ(submitted.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(manager->repository_size(), 0U);
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerTest, SubmitsExecutesAndQueriesTask) {
    QuietLogger logger;
    auto manager = make_manager(
        logger,
        [](const TaskRequest& request) {
            return Result<nlohmann::json>::success(request.input);
        });
    ASSERT_NE(manager, nullptr);

    auto submitted = manager->submit(TaskRequest{"echo", {{"value", 5}}});
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(manager->shutdown());
    auto snapshot = manager->get_snapshot(submitted.value());

    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Succeeded);
    EXPECT_EQ(snapshot.value().result->at("value"), 5);
}

TEST(TaskManagerTest, HandlerReceivesIndependentRequestCopy) {
    QuietLogger logger;
    std::promise<void> blocker_started;
    auto blocker_started_future = blocker_started.get_future();
    std::promise<void> release_blocker;
    auto release_blocker_future = release_blocker.get_future().share();
    std::promise<int> observed_value;
    auto observed_value_future = observed_value.get_future();
    auto manager = make_manager(
        logger,
        [&blocker_started, release_blocker_future, &observed_value](
            const TaskRequest& request) {
            if (request.operation == "block") {
                blocker_started.set_value();
                release_blocker_future.wait();
            } else {
                observed_value.set_value(request.input.at("value").get<int>());
            }
            return Result<nlohmann::json>::success(request.input);
        },
        ThreadPoolOptions{1, 2});
    ASSERT_NE(manager, nullptr);
    ASSERT_TRUE(manager->submit(TaskRequest{"block", {}}));
    ASSERT_EQ(
        blocker_started_future.wait_for(2s),
        std::future_status::ready);

    TaskRequest caller_owned{"observe", {{"value", 7}}};
    const auto submitted = manager->submit(caller_owned);
    ASSERT_TRUE(submitted);
    caller_owned.operation = "mutated";
    caller_owned.input["value"] = 99;
    release_blocker.set_value();

    ASSERT_EQ(
        observed_value_future.wait_for(2s),
        std::future_status::ready);
    EXPECT_EQ(observed_value_future.get(), 7);
    ASSERT_TRUE(manager->shutdown());
    EXPECT_EQ(
        manager->get_snapshot(submitted.value()).value().result->at("value"),
        7);
}

TEST(TaskManagerTest, HandlerCallsCanRunConcurrently) {
    QuietLogger logger;
    std::atomic<int> active{0};
    std::atomic<int> maximum_active{0};
    std::promise<void> all_started;
    auto all_started_future = all_started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto manager = make_manager(
        logger,
        [&active, &maximum_active, &all_started, release_future](
            const TaskRequest&) {
            const int now =
                active.fetch_add(1, std::memory_order_relaxed) + 1;
            int observed = maximum_active.load(std::memory_order_relaxed);
            while (now > observed &&
                   !maximum_active.compare_exchange_weak(
                       observed,
                       now,
                       std::memory_order_relaxed)) {
            }
            if (now == 3) {
                all_started.set_value();
            }
            release_future.wait();
            active.fetch_sub(1, std::memory_order_relaxed);
            return Result<nlohmann::json>::success({});
        },
        ThreadPoolOptions{3, 3});
    ASSERT_NE(manager, nullptr);
    for (int index = 0; index < 3; ++index) {
        ASSERT_TRUE(manager->submit(TaskRequest{"parallel", {{"i", index}}}));
    }

    ASSERT_EQ(all_started_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(maximum_active.load(std::memory_order_relaxed), 3);
    release.set_value();
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerTest, QueueRejectionRollsBackRepositoryRecord) {
    QuietLogger logger;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::atomic<int> calls{0};
    auto manager = make_manager(
        logger,
        [&started, release_future, &calls](const TaskRequest&) {
            if (calls.fetch_add(1, std::memory_order_relaxed) == 0) {
                started.set_value();
                release_future.wait();
            }
            return Result<nlohmann::json>::success({{"ok", true}});
        },
        ThreadPoolOptions{1, 1});
    ASSERT_NE(manager, nullptr);

    ASSERT_TRUE(manager->submit(TaskRequest{"first", {}}));
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(manager->submit(TaskRequest{"second", {}}));
    auto rejected = manager->submit(TaskRequest{"third", {}});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::ResourceExhausted);
    const auto typed_rejection =
        manager->submit_with_outcome(TaskRequest{"fourth", {}});
    EXPECT_FALSE(typed_rejection.result);
    EXPECT_EQ(
        typed_rejection.failure,
        iaisf::task::TaskSubmitFailure::QueueCapacity);
    EXPECT_EQ(manager->repository_size(), 2U);
    EXPECT_EQ(
        manager->get_snapshot(iaisf::task::TaskId{3}).error().code,
        ErrorCode::NotFound);
    release.set_value();
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerTest, RepositoryCapacityRejectsWithoutEviction) {
    QuietLogger logger;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto manager = make_manager(
        logger,
        [&started, release_future](const TaskRequest&) {
            started.set_value();
            release_future.wait();
            return Result<nlohmann::json>::success({{"ok", true}});
        },
        ThreadPoolOptions{1, 4},
        TaskLimits::create(1).value());
    ASSERT_NE(manager, nullptr);
    auto first = manager->submit(TaskRequest{"first", {}});
    ASSERT_TRUE(first);
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);

    auto second = manager->submit_with_outcome(TaskRequest{"second", {}});

    ASSERT_FALSE(second.result);
    EXPECT_EQ(second.result.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(
        second.failure,
        iaisf::task::TaskSubmitFailure::RepositoryCapacity);
    EXPECT_EQ(manager->repository_size(), 1U);
    release.set_value();
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerTest, QueueFullAndShutdownRaceLeavesNoOrphanRecord) {
    QuietLogger logger;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release_work;
    auto release_work_future = release_work.get_future().share();
    std::atomic<int> calls{0};
    auto manager = make_manager(
        logger,
        [&started, release_work_future, &calls](const TaskRequest&) {
            if (calls.fetch_add(1, std::memory_order_relaxed) == 0) {
                started.set_value();
                release_work_future.wait();
            }
            return Result<nlohmann::json>::success({});
        },
        ThreadPoolOptions{1, 1});
    ASSERT_NE(manager, nullptr);
    const auto first = manager->submit(TaskRequest{"first", {}});
    ASSERT_TRUE(first);
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    const auto second = manager->submit(TaskRequest{"second", {}});
    ASSERT_TRUE(second);

    std::promise<void> release_race;
    const auto race_gate = release_race.get_future().share();
    std::atomic<int> rejected_code{-1};
    std::atomic<bool> stopped{false};
    std::thread submitter{[&] {
        race_gate.wait();
        const auto result = manager->submit(TaskRequest{"third", {}});
        rejected_code.store(
            result ? 0 : static_cast<int>(result.error().code),
            std::memory_order_relaxed);
    }};
    std::thread stopper{[&] {
        race_gate.wait();
        stopped.store(
            static_cast<bool>(manager->shutdown()),
            std::memory_order_relaxed);
    }};

    release_race.set_value();
    submitter.join();
    release_work.set_value();
    stopper.join();

    const int code = rejected_code.load(std::memory_order_relaxed);
    EXPECT_TRUE(
        code == static_cast<int>(ErrorCode::ResourceExhausted) ||
        code == static_cast<int>(ErrorCode::InvalidState));
    EXPECT_TRUE(stopped.load(std::memory_order_relaxed));
    EXPECT_EQ(manager->repository_size(), 2U);
    EXPECT_EQ(
        manager->get_snapshot(iaisf::task::TaskId{3}).error().code,
        ErrorCode::NotFound);
    EXPECT_TRUE(iaisf::task::is_terminal(
        manager->get_snapshot(first.value()).value().state));
    EXPECT_TRUE(iaisf::task::is_terminal(
        manager->get_snapshot(second.value()).value().state));
}

TEST(TaskManagerTest, RepositoryFullAndShutdownRaceLeavesNoOrphanRecord) {
    QuietLogger logger;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release_work;
    auto release_work_future = release_work.get_future().share();
    auto manager = make_manager(
        logger,
        [&started, release_work_future](const TaskRequest&) {
            started.set_value();
            release_work_future.wait();
            return Result<nlohmann::json>::success({});
        },
        ThreadPoolOptions{1, 2},
        TaskLimits::create(1).value());
    ASSERT_NE(manager, nullptr);
    const auto first = manager->submit(TaskRequest{"first", {}});
    ASSERT_TRUE(first);
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);

    std::promise<void> release_race;
    const auto race_gate = release_race.get_future().share();
    std::atomic<int> rejected_code{-1};
    std::atomic<bool> stopped{false};
    std::thread submitter{[&] {
        race_gate.wait();
        const auto result = manager->submit(TaskRequest{"second", {}});
        rejected_code.store(
            result ? 0 : static_cast<int>(result.error().code),
            std::memory_order_relaxed);
    }};
    std::thread stopper{[&] {
        race_gate.wait();
        stopped.store(
            static_cast<bool>(manager->shutdown()),
            std::memory_order_relaxed);
    }};

    release_race.set_value();
    submitter.join();
    release_work.set_value();
    stopper.join();

    const int code = rejected_code.load(std::memory_order_relaxed);
    EXPECT_TRUE(
        code == static_cast<int>(ErrorCode::ResourceExhausted) ||
        code == static_cast<int>(ErrorCode::InvalidState));
    EXPECT_TRUE(stopped.load(std::memory_order_relaxed));
    EXPECT_EQ(manager->repository_size(), 1U);
    EXPECT_TRUE(iaisf::task::is_terminal(
        manager->get_snapshot(first.value()).value().state));
}

TEST(TaskManagerTest, ShutdownDrainsAndRejectsNewSubmissions) {
    QuietLogger logger;
    std::atomic<int> handled{0};
    auto manager = make_manager(
        logger,
        [&handled](const TaskRequest&) {
            handled.fetch_add(1, std::memory_order_relaxed);
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);
    for (int index = 0; index < 8; ++index) {
        ASSERT_TRUE(manager->submit(TaskRequest{"work", {{"index", index}}}));
    }

    EXPECT_TRUE(manager->shutdown());
    EXPECT_EQ(handled.load(std::memory_order_relaxed), 8);
    EXPECT_TRUE(manager->shutdown());
    EXPECT_FALSE(manager->accepting());
    auto rejected = manager->submit(TaskRequest{"late", {}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidState);
}

TEST(TaskManagerTest, ShutdownWaitsForInFlightSubmissionBarrier) {
    QuietLogger logger;
    auto manager = make_manager(
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);
    ASSERT_TRUE(
        iaisf::task::TaskManagerTestAccess::begin_submission(*manager));

    auto shutdown = std::async(
        std::launch::async,
        [&manager] { return manager->shutdown(); });
    iaisf::task::TaskManagerTestAccess::wait_until_admission_closes(*manager);

    EXPECT_EQ(shutdown.wait_for(0s), std::future_status::timeout);
    const auto rejected = manager->submit(TaskRequest{"late", {}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidState);

    iaisf::task::TaskManagerTestAccess::finish_submission(*manager);
    ASSERT_EQ(shutdown.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(shutdown.get());
}

TEST(TaskManagerTest, ConcurrentShutdownCallersDrainBeforeDestructor) {
    QuietLogger logger;
    std::promise<void> handler_started;
    auto handler_started_future = handler_started.get_future();
    std::promise<void> release_handler;
    auto release_handler_future = release_handler.get_future().share();
    auto manager = make_manager(
        logger,
        [&handler_started, release_handler_future](const TaskRequest&) {
            handler_started.set_value();
            release_handler_future.wait();
            return Result<nlohmann::json>::success({});
        },
        ThreadPoolOptions{1, 2});
    ASSERT_NE(manager, nullptr);
    const auto submitted = manager->submit(TaskRequest{"blocked", {}});
    ASSERT_TRUE(submitted);
    ASSERT_EQ(
        handler_started_future.wait_for(2s),
        std::future_status::ready);

    constexpr std::size_t caller_count = 6;
    std::promise<void> release_callers;
    const auto gate = release_callers.get_future().share();
    std::promise<void> all_callers_started;
    auto all_callers_started_future = all_callers_started.get_future();
    std::atomic<std::size_t> started_count{0};
    std::array<std::atomic<bool>, caller_count> results{};
    for (auto& result : results) {
        result.store(false, std::memory_order_relaxed);
    }
    std::vector<std::thread> callers;
    for (std::size_t index = 0; index < caller_count; ++index) {
        callers.emplace_back([&, index] {
            gate.wait();
            if (started_count.fetch_add(1, std::memory_order_relaxed) + 1 ==
                caller_count) {
                all_callers_started.set_value();
            }
            results[index].store(
                static_cast<bool>(manager->shutdown()),
                std::memory_order_relaxed);
        });
    }

    release_callers.set_value();
    ASSERT_EQ(
        all_callers_started_future.wait_for(2s),
        std::future_status::ready);
    release_handler.set_value();
    for (auto& caller : callers) {
        caller.join();
    }

    for (const auto& result : results) {
        EXPECT_TRUE(result.load(std::memory_order_relaxed));
    }
    EXPECT_TRUE(iaisf::task::is_terminal(
        manager->get_snapshot(submitted.value()).value().state));
    manager.reset();
}

TEST(TaskManagerTest, HandlerExceptionFailsTaskAndWorkerSurvives) {
    QuietLogger logger;
    std::atomic<int> calls{0};
    auto manager = make_manager(
        logger,
        [&calls](const TaskRequest&) -> Result<nlohmann::json> {
            if (calls.fetch_add(1, std::memory_order_relaxed) == 0) {
                throw std::runtime_error{"handler"};
            }
            return Result<nlohmann::json>::success({{"ok", true}});
        },
        ThreadPoolOptions{1, 4});
    ASSERT_NE(manager, nullptr);
    auto first = manager->submit(TaskRequest{"first", {}});
    auto second = manager->submit(TaskRequest{"second", {}});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(manager->shutdown());

    EXPECT_EQ(
        manager->get_snapshot(first.value()).value().state,
        TaskState::Failed);
    EXPECT_EQ(
        manager->get_snapshot(second.value()).value().state,
        TaskState::Succeeded);
    EXPECT_EQ(manager->handler_exception_count(), 1U);
}

TEST(TaskManagerTest, ExplicitHandlerFailureCreatesFailedTerminalTask) {
    QuietLogger logger;
    auto manager = make_manager(
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::failure(
                make_error(ErrorCode::InvalidArgument, "rejected by handler"));
        });
    ASSERT_NE(manager, nullptr);
    auto submitted = manager->submit(TaskRequest{"fail", {}});
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(manager->shutdown());

    const auto snapshot = manager->get_snapshot(submitted.value());
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::Failed);
    EXPECT_EQ(snapshot.value().error->message, "rejected by handler");
}

TEST(TaskManagerTest, TimeoutWinsAgainstLateHandlerResult) {
    QuietLogger logger;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto manager = make_manager(
        logger,
        [&started, release_future](const TaskRequest&) {
            started.set_value();
            release_future.wait();
            return Result<nlohmann::json>::success({{"late", true}});
        },
        ThreadPoolOptions{1, 2});
    ASSERT_NE(manager, nullptr);
    auto submitted = manager->submit(TaskRequest{"slow", {}});
    ASSERT_TRUE(submitted);
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);

    ASSERT_TRUE(manager->mark_timed_out(
        submitted.value(), make_error(ErrorCode::InvalidState, "deadline")));
    release.set_value();
    ASSERT_TRUE(manager->shutdown());

    auto snapshot = manager->get_snapshot(submitted.value());
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state, TaskState::TimedOut);
    EXPECT_EQ(manager->late_completion_count(), 1U);
}

TEST(TaskManagerTest, TimedOutEraseDropsLateResultAndWorkerContinues) {
    QuietLogger logger;
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto manager = make_manager(
        logger,
        [&started, release_future](const TaskRequest& request) {
            if (request.operation == "slow") {
                started.set_value();
                release_future.wait();
            }
            return Result<nlohmann::json>::success({{"operation", request.operation}});
        },
        ThreadPoolOptions{1, 2},
        TaskLimits::create(1).value());
    ASSERT_NE(manager, nullptr);
    const auto slow = manager->submit(TaskRequest{"slow", {}});
    ASSERT_TRUE(slow);
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(manager->mark_timed_out(slow.value()));
    ASSERT_TRUE(manager->erase_terminal(slow.value()));

    const auto next = manager->submit(TaskRequest{"next", {}});
    ASSERT_TRUE(next);
    release.set_value();
    ASSERT_TRUE(manager->shutdown());

    EXPECT_EQ(
        manager->get_snapshot(slow.value()).error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(
        manager->get_snapshot(next.value()).value().state,
        TaskState::Succeeded);
    EXPECT_EQ(manager->late_completion_count(), 1U);
}

TEST(TaskManagerTest, ConcurrentSubmissionsReceiveUniqueIds) {
    QuietLogger logger;
    auto manager = make_manager(
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({{"ok", true}});
        },
        ThreadPoolOptions{4, 128});
    ASSERT_NE(manager, nullptr);
    std::mutex ids_mutex;
    std::vector<std::uint64_t> ids;
    std::vector<std::thread> submitters;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        submitters.emplace_back([&, thread_index] {
            for (int index = 0; index < 10; ++index) {
                auto submitted = manager->submit(
                    TaskRequest{"parallel", {{"thread", thread_index}, {"i", index}}});
                if (submitted) {
                    std::lock_guard<std::mutex> lock(ids_mutex);
                    ids.push_back(submitted.value().value());
                }
            }
        });
    }
    for (auto& submitter : submitters) {
        submitter.join();
    }
    ASSERT_TRUE(manager->shutdown());

    ASSERT_EQ(ids.size(), 40U);
    const std::set<std::uint64_t> unique(ids.begin(), ids.end());
    EXPECT_EQ(unique.size(), ids.size());
    EXPECT_EQ(manager->repository_size(), ids.size());
}

TEST(TaskManagerTest, ConcurrentSubmitAndShutdownFullyAcceptsOrRejectsEachTask) {
    QuietLogger logger;
    auto manager = make_manager(
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({{"ok", true}});
        },
        ThreadPoolOptions{4, 128},
        TaskLimits::create(256).value());
    ASSERT_NE(manager, nullptr);
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::mutex results_mutex;
    std::vector<iaisf::task::TaskId> accepted;
    std::vector<ErrorCode> rejected;
    std::atomic<bool> stop_succeeded{false};
    std::vector<std::thread> submitters;
    for (int thread_index = 0; thread_index < 4; ++thread_index) {
        submitters.emplace_back([&, thread_index] {
            gate.wait();
            for (int index = 0; index < 20; ++index) {
                auto submitted = manager->submit(
                    TaskRequest{"race", {{"thread", thread_index}, {"i", index}}});
                std::lock_guard<std::mutex> lock(results_mutex);
                if (submitted) {
                    accepted.push_back(submitted.value());
                } else {
                    rejected.push_back(submitted.error().code);
                }
            }
        });
    }
    std::thread stopper{[&] {
        gate.wait();
        const auto result = manager->shutdown();
        stop_succeeded.store(static_cast<bool>(result), std::memory_order_relaxed);
    }};
    release.set_value();
    for (auto& submitter : submitters) {
        submitter.join();
    }
    stopper.join();
    ASSERT_TRUE(manager->shutdown());

    EXPECT_TRUE(stop_succeeded.load(std::memory_order_relaxed));
    EXPECT_EQ(accepted.size() + rejected.size(), 80U);
    EXPECT_TRUE(std::all_of(
        rejected.begin(),
        rejected.end(),
        [](const ErrorCode code) { return code == ErrorCode::InvalidState; }));
    EXPECT_EQ(manager->repository_size(), accepted.size());
    for (const auto id : accepted) {
        const auto snapshot = manager->get_snapshot(id);
        ASSERT_TRUE(snapshot);
        EXPECT_TRUE(iaisf::task::is_terminal(snapshot.value().state));
    }
}

TEST(TaskManagerTest, EraseTerminalReleasesRepositoryCapacity) {
    QuietLogger logger;
    auto manager = make_manager(
        logger,
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);
    auto submitted = manager->submit(TaskRequest{"erase", {}});
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(manager->shutdown());
    ASSERT_TRUE(manager->erase_terminal(submitted.value()));

    EXPECT_EQ(manager->repository_size(), 0U);
    EXPECT_EQ(
        manager->get_snapshot(submitted.value()).error().code,
        ErrorCode::NotFound);
}

TEST(TaskManagerTest, WorkerInitiatedShutdownFailsWithoutStoppingAdmission) {
    QuietLogger logger;
    TaskManager* manager_pointer = nullptr;
    std::promise<ErrorCode> observed;
    auto observed_future = observed.get_future();
    auto manager = make_manager(
        logger,
        [&manager_pointer, &observed](const TaskRequest&) {
            const auto result = manager_pointer->shutdown();
            observed.set_value(result ? ErrorCode::InternalError : result.error().code);
            return Result<nlohmann::json>::success({});
        },
        ThreadPoolOptions{1, 2});
    ASSERT_NE(manager, nullptr);
    manager_pointer = manager.get();

    ASSERT_TRUE(manager->submit(TaskRequest{"self-stop", {}}));
    ASSERT_EQ(observed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(observed_future.get(), ErrorCode::InvalidState);
    EXPECT_TRUE(manager->accepting());
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerTest, DestructorDrainsAcceptedWorkBeforeDestroyingDependencies) {
    QuietLogger logger;
    std::atomic<int> executions{0};
    {
        auto manager = make_manager(
            logger,
            [&executions](const TaskRequest&) {
                executions.fetch_add(1, std::memory_order_relaxed);
                return Result<nlohmann::json>::success({});
            },
            ThreadPoolOptions{2, 16});
        ASSERT_NE(manager, nullptr);
        for (int index = 0; index < 12; ++index) {
            ASSERT_TRUE(manager->submit(TaskRequest{"drain", {{"i", index}}}));
        }
    }

    EXPECT_EQ(executions.load(std::memory_order_relaxed), 12);
}

TEST(TaskManagerTest, DestructorWaitsForMultipleRunningHandlers) {
    QuietLogger logger;
    std::atomic<int> started_count{0};
    std::atomic<int> completed_count{0};
    std::promise<void> all_started;
    auto all_started_future = all_started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto manager = make_manager(
        logger,
        [&started_count, &completed_count, &all_started, release_future](
            const TaskRequest&) {
            if (started_count.fetch_add(1, std::memory_order_relaxed) + 1 == 2) {
                all_started.set_value();
            }
            release_future.wait();
            completed_count.fetch_add(1, std::memory_order_relaxed);
            return Result<nlohmann::json>::success({});
        },
        ThreadPoolOptions{2, 2});
    ASSERT_NE(manager, nullptr);
    ASSERT_TRUE(manager->submit(TaskRequest{"first", {}}));
    ASSERT_TRUE(manager->submit(TaskRequest{"second", {}}));
    ASSERT_EQ(all_started_future.wait_for(2s), std::future_status::ready);

    std::thread destroyer{
        [owned = std::move(manager)]() mutable { owned.reset(); }};
    release.set_value();
    destroyer.join();

    EXPECT_EQ(completed_count.load(std::memory_order_relaxed), 2);
}

TEST(TaskManagerValidatorTest, SuccessRunsBeforeTaskCreationAndExecution) {
    QuietLogger logger;
    std::atomic<int> validations{0};
    std::atomic<int> executions{0};
    auto manager = make_validating_manager(
        logger,
        [&validations](const TaskRequest&) {
            validations.fetch_add(1, std::memory_order_relaxed);
            return Result<void>::success();
        },
        [&executions](const TaskRequest&) {
            executions.fetch_add(1, std::memory_order_relaxed);
            return Result<nlohmann::json>::success({{"ok", true}});
        });
    ASSERT_NE(manager, nullptr);

    const auto submitted = manager->submit(TaskRequest{"validated", {}});
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(manager->shutdown());

    EXPECT_EQ(validations.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(executions.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(
        manager->get_snapshot(submitted.value()).value().state,
        TaskState::Succeeded);
}

TEST(TaskManagerValidatorTest, FailurePreservesIdRepositoryAndQueueCapacity) {
    QuietLogger logger;
    auto manager = make_validating_manager(
        logger,
        [](const TaskRequest& request) {
            if (request.operation == "reject") {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidArgument,
                    "request rejected"));
            }
            return Result<void>::success();
        },
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        },
        ThreadPoolOptions{1, 1},
        TaskLimits::create(1).value());
    ASSERT_NE(manager, nullptr);

    const auto rejected = manager->submit(TaskRequest{"reject", {}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(manager->repository_size(), 0U);
    EXPECT_EQ(manager->pending_count(), 0U);

    const auto accepted = manager->submit(TaskRequest{"accept", {}});
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted.value().value(), 1U);
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerValidatorTest, PropagatesStructuredNotFoundWithoutTask) {
    QuietLogger logger;
    auto manager = make_validating_manager(
        logger,
        [](const TaskRequest&) {
            return Result<void>::failure(make_error(
                ErrorCode::NotFound,
                "operation was not found"));
        },
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);

    const auto rejected = manager->submit(TaskRequest{"missing", {}});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::NotFound);
    EXPECT_EQ(manager->repository_size(), 0U);
    EXPECT_EQ(manager->pending_count(), 0U);
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerValidatorTest, StandardExceptionUsesFixedInternalError) {
    QuietLogger logger;
    auto manager = make_validating_manager(
        logger,
        [](const TaskRequest&) -> Result<void> {
            throw std::runtime_error("validator secret");
        },
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);

    const auto rejected = manager->submit(TaskRequest{"throwing", {}});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InternalError);
    EXPECT_EQ(rejected.error().message, "task validation failed");
    EXPECT_EQ(rejected.error().message.find("secret"), std::string::npos);
    EXPECT_EQ(manager->repository_size(), 0U);
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerValidatorTest, UnknownExceptionUsesFixedInternalError) {
    QuietLogger logger;
    auto manager = make_validating_manager(
        logger,
        [](const TaskRequest&) -> Result<void> { throw 42; },
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);

    const auto rejected = manager->submit(TaskRequest{"throwing", {}});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InternalError);
    EXPECT_EQ(rejected.error().message, "task validation failed");
    EXPECT_EQ(manager->repository_size(), 0U);
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerValidatorTest, GenericValidationRunsBeforeCustomValidator) {
    QuietLogger logger;
    std::atomic<int> validator_calls{0};
    auto manager = make_validating_manager(
        logger,
        [&validator_calls](const TaskRequest&) {
            validator_calls.fetch_add(1, std::memory_order_relaxed);
            return Result<void>::success();
        },
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);

    const auto rejected = manager->submit(TaskRequest{"   ", {}});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(validator_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(manager->repository_size(), 0U);
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerValidatorTest, ShutdownWaitsForInflightValidator) {
    QuietLogger logger;
    std::promise<void> validator_entered;
    auto validator_entered_future = validator_entered.get_future();
    std::promise<void> release_validator;
    auto release_validator_future = release_validator.get_future().share();
    auto manager = make_validating_manager(
        logger,
        [&validator_entered, release_validator_future](const TaskRequest&) {
            validator_entered.set_value();
            release_validator_future.wait();
            return Result<void>::success();
        },
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);

    auto submission = std::async(
        std::launch::async,
        [&manager] { return manager->submit(TaskRequest{"blocked", {}}); });
    ASSERT_EQ(
        validator_entered_future.wait_for(2s),
        std::future_status::ready);
    auto shutdown = std::async(
        std::launch::async,
        [&manager] { return manager->shutdown(); });
    iaisf::task::TaskManagerTestAccess::wait_until_admission_closes(*manager);
    EXPECT_EQ(shutdown.wait_for(0ms), std::future_status::timeout);

    release_validator.set_value();
    ASSERT_TRUE(submission.get());
    EXPECT_TRUE(shutdown.get());
}

TEST(TaskManagerValidatorTest, ConcurrentSubmissionsRunValidatorConcurrently) {
    QuietLogger logger;
    std::atomic<int> active{0};
    std::atomic<int> maximum_active{0};
    std::promise<void> all_entered;
    auto all_entered_future = all_entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    auto manager = make_validating_manager(
        logger,
        [&active, &maximum_active, &all_entered, release_future](
            const TaskRequest&) {
            const int current =
                active.fetch_add(1, std::memory_order_relaxed) + 1;
            int observed = maximum_active.load(std::memory_order_relaxed);
            while (current > observed &&
                   !maximum_active.compare_exchange_weak(
                       observed,
                       current,
                       std::memory_order_relaxed)) {
            }
            if (current == 3) {
                all_entered.set_value();
            }
            release_future.wait();
            active.fetch_sub(1, std::memory_order_relaxed);
            return Result<void>::success();
        },
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        },
        ThreadPoolOptions{3, 3});
    ASSERT_NE(manager, nullptr);

    std::array<std::future<Result<iaisf::task::TaskId>>, 3> submissions{
        std::async(
            std::launch::async,
            [&manager] { return manager->submit(TaskRequest{"one", {}}); }),
        std::async(
            std::launch::async,
            [&manager] { return manager->submit(TaskRequest{"two", {}}); }),
        std::async(
            std::launch::async,
            [&manager] { return manager->submit(TaskRequest{"three", {}}); }),
    };
    ASSERT_EQ(all_entered_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(maximum_active.load(std::memory_order_relaxed), 3);
    release.set_value();
    for (auto& submission : submissions) {
        EXPECT_TRUE(submission.get());
    }
    EXPECT_TRUE(manager->shutdown());
}

TEST(TaskManagerValidatorTest, ValidatorRunsOutsideManagerAndRepositoryLocks) {
    QuietLogger logger;
    TaskManager* observed_manager = nullptr;
    std::atomic<bool> inspected{false};
    auto manager = make_validating_manager(
        logger,
        [&observed_manager, &inspected](const TaskRequest&) {
            const bool accepting = observed_manager->accepting();
            const auto repository_size = observed_manager->repository_size();
            inspected.store(
                accepting && repository_size == 0U,
                std::memory_order_relaxed);
            return Result<void>::success();
        },
        [](const TaskRequest&) {
            return Result<nlohmann::json>::success({});
        });
    ASSERT_NE(manager, nullptr);
    observed_manager = manager.get();

    auto submission = std::async(
        std::launch::async,
        [&manager] { return manager->submit(TaskRequest{"inspect", {}}); });
    ASSERT_EQ(submission.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(submission.get());
    EXPECT_TRUE(inspected.load(std::memory_order_relaxed));
    EXPECT_TRUE(manager->shutdown());
}

}  // namespace
