#include <atomic>
#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "iaisf/task/thread_pool.hpp"

namespace {

using namespace std::chrono_literals;
using iaisf::ErrorCode;
using iaisf::task::BoundedThreadPool;
using iaisf::task::ThreadPoolOptions;

std::unique_ptr<BoundedThreadPool> make_pool(
    const std::size_t workers = 1,
    const std::size_t capacity = 8) {
    auto created =
        BoundedThreadPool::create(ThreadPoolOptions{workers, capacity});
    EXPECT_TRUE(created);
    return created ? std::move(created).value() : nullptr;
}

TEST(BoundedThreadPoolTest, RejectsInvalidOptions) {
    auto no_workers = BoundedThreadPool::create(ThreadPoolOptions{0, 1});
    auto too_many_workers = BoundedThreadPool::create(ThreadPoolOptions{257, 1});
    auto no_queue = BoundedThreadPool::create(ThreadPoolOptions{1, 0});
    auto huge_queue = BoundedThreadPool::create(ThreadPoolOptions{1, 1000001});

    EXPECT_FALSE(no_workers);
    EXPECT_FALSE(too_many_workers);
    EXPECT_FALSE(no_queue);
    EXPECT_FALSE(huge_queue);
    EXPECT_EQ(no_workers.error().code, ErrorCode::InvalidArgument);
}

TEST(BoundedThreadPoolTest, RejectsEmptyWorkItem) {
    auto pool = make_pool();
    ASSERT_NE(pool, nullptr);

    auto submitted = pool->try_submit({});

    EXPECT_FALSE(submitted);
    EXPECT_EQ(submitted.error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(pool->shutdown());
}

TEST(BoundedThreadPoolTest, ExecutesAcceptedWork) {
    auto pool = make_pool(2, 4);
    ASSERT_NE(pool, nullptr);
    std::promise<void> completed;
    auto completed_future = completed.get_future();

    ASSERT_TRUE(pool->try_submit([&completed] { completed.set_value(); }));
    EXPECT_EQ(completed_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(pool->shutdown());
}

TEST(BoundedThreadPoolTest, RejectsWhenBoundedQueueIsFull) {
    auto pool = make_pool(1, 1);
    ASSERT_NE(pool, nullptr);
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();

    ASSERT_TRUE(pool->try_submit([&started, release_future] {
        started.set_value();
        release_future.wait();
    }));
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(pool->try_submit([] {}));

    auto rejected = pool->try_submit([] {});
    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(pool->pending_count(), 1U);

    release.set_value();
    EXPECT_TRUE(pool->shutdown());
}

TEST(BoundedThreadPoolTest, WorkExceptionDoesNotTerminateWorker) {
    auto pool = make_pool();
    ASSERT_NE(pool, nullptr);
    std::promise<void> survived;
    auto survived_future = survived.get_future();

    ASSERT_TRUE(pool->try_submit([] { throw std::runtime_error{"boom"}; }));
    ASSERT_TRUE(pool->try_submit([&survived] { survived.set_value(); }));

    EXPECT_EQ(survived_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(pool->shutdown());
    EXPECT_EQ(pool->task_exception_count(), 1U);
}

TEST(BoundedThreadPoolTest, UnknownWorkExceptionDoesNotTerminateWorker) {
    auto pool = make_pool();
    ASSERT_NE(pool, nullptr);
    std::promise<void> survived;
    auto survived_future = survived.get_future();

    ASSERT_TRUE(pool->try_submit([] { throw 17; }));
    ASSERT_TRUE(pool->try_submit([&survived] { survived.set_value(); }));

    EXPECT_EQ(survived_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(pool->shutdown());
    EXPECT_EQ(pool->unhandled_exception_count(), 1U);
}

TEST(BoundedThreadPoolTest, PreservesFifoOrderWithOneWorker) {
    auto pool = make_pool(1, 8);
    ASSERT_NE(pool, nullptr);
    std::promise<void> started;
    auto started_future = started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    std::mutex order_mutex;
    std::vector<int> order;
    ASSERT_TRUE(pool->try_submit([&started, release_future] {
        started.set_value();
        release_future.wait();
    }));
    ASSERT_EQ(started_future.wait_for(2s), std::future_status::ready);
    for (int index = 0; index < 5; ++index) {
        ASSERT_TRUE(pool->try_submit([index, &order_mutex, &order] {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(index);
        }));
    }

    release.set_value();
    ASSERT_TRUE(pool->shutdown());
    EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(BoundedThreadPoolTest, StartsConfiguredWorkersAndRunsConcurrently) {
    auto pool = make_pool(3, 3);
    ASSERT_NE(pool, nullptr);
    std::atomic<int> started_count{0};
    std::promise<void> all_started;
    auto all_started_future = all_started.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    for (int index = 0; index < 3; ++index) {
        ASSERT_TRUE(pool->try_submit([&started_count, &all_started, release_future] {
            if (started_count.fetch_add(1, std::memory_order_relaxed) + 1 == 3) {
                all_started.set_value();
            }
            release_future.wait();
        }));
    }

    EXPECT_EQ(all_started_future.wait_for(2s), std::future_status::ready);
    release.set_value();
    EXPECT_TRUE(pool->shutdown());
}

TEST(BoundedThreadPoolTest, WorkCanSubmitMoreWorkWithoutQueueLockDeadlock) {
    auto pool = make_pool(1, 2);
    ASSERT_NE(pool, nullptr);
    auto* const raw_pool = pool.get();
    std::promise<bool> nested_submit;
    auto nested_submit_future = nested_submit.get_future();
    std::promise<void> nested_ran;
    auto nested_ran_future = nested_ran.get_future();

    ASSERT_TRUE(pool->try_submit([raw_pool, &nested_submit, &nested_ran] {
        auto result =
            raw_pool->try_submit([&nested_ran] { nested_ran.set_value(); });
        nested_submit.set_value(static_cast<bool>(result));
    }));

    ASSERT_EQ(nested_submit_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(nested_submit_future.get());
    EXPECT_EQ(nested_ran_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(pool->shutdown());
}

TEST(BoundedThreadPoolTest, MultipleProducersExecuteAcceptedWorkExactlyOnce) {
    auto pool = make_pool(4, 128);
    ASSERT_NE(pool, nullptr);
    std::array<std::atomic<int>, 100> executions{};
    for (auto& execution : executions) {
        execution.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> rejected{0};
    std::vector<std::thread> producers;
    for (std::size_t producer = 0; producer < 4; ++producer) {
        producers.emplace_back([producer, &executions, &rejected, raw = pool.get()] {
            for (std::size_t offset = 0; offset < 25; ++offset) {
                const std::size_t index = producer * 25 + offset;
                auto submitted = raw->try_submit([index, &executions] {
                    executions[index].fetch_add(1, std::memory_order_relaxed);
                });
                if (!submitted) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    ASSERT_TRUE(pool->shutdown());

    EXPECT_EQ(rejected.load(std::memory_order_relaxed), 0);
    for (const auto& execution : executions) {
        EXPECT_EQ(execution.load(std::memory_order_relaxed), 1);
    }
}

TEST(BoundedThreadPoolTest, ShutdownDrainsAcceptedWorkAndIsIdempotent) {
    auto pool = make_pool(2, 8);
    ASSERT_NE(pool, nullptr);
    std::atomic<int> executions{0};
    for (int index = 0; index < 6; ++index) {
        ASSERT_TRUE(pool->try_submit(
            [&executions] { executions.fetch_add(1, std::memory_order_relaxed); }));
    }

    EXPECT_TRUE(pool->shutdown());
    EXPECT_EQ(executions.load(std::memory_order_relaxed), 6);
    EXPECT_EQ(pool->pending_count(), 0U);
    EXPECT_TRUE(pool->stopped());
    EXPECT_TRUE(pool->shutdown());
    EXPECT_FALSE(pool->accepting());
}

TEST(BoundedThreadPoolTest, ConcurrentShutdownHasOneJoinerAndAllCallersSucceed) {
    auto pool = make_pool(2, 8);
    ASSERT_NE(pool, nullptr);
    std::promise<void> work_started;
    auto work_started_future = work_started.get_future();
    std::promise<void> release_work;
    auto release_work_future = release_work.get_future().share();
    ASSERT_TRUE(pool->try_submit([&work_started, release_work_future] {
        work_started.set_value();
        release_work_future.wait();
    }));
    ASSERT_EQ(work_started_future.wait_for(2s), std::future_status::ready);

    constexpr std::size_t caller_count = 8;
    std::promise<void> release_callers;
    const auto caller_gate = release_callers.get_future().share();
    std::promise<void> all_callers_started;
    auto all_callers_started_future = all_callers_started.get_future();
    std::atomic<std::size_t> callers_started{0};
    std::array<std::atomic<bool>, caller_count> results{};
    for (auto& result : results) {
        result.store(false, std::memory_order_relaxed);
    }
    std::vector<std::thread> callers;
    callers.reserve(caller_count);
    for (std::size_t index = 0; index < caller_count; ++index) {
        callers.emplace_back([&, index] {
            caller_gate.wait();
            if (callers_started.fetch_add(1, std::memory_order_relaxed) + 1 ==
                caller_count) {
                all_callers_started.set_value();
            }
            results[index].store(
                static_cast<bool>(pool->shutdown()),
                std::memory_order_relaxed);
        });
    }

    release_callers.set_value();
    ASSERT_EQ(
        all_callers_started_future.wait_for(2s),
        std::future_status::ready);
    release_work.set_value();
    for (auto& caller : callers) {
        caller.join();
    }

    for (const auto& result : results) {
        EXPECT_TRUE(result.load(std::memory_order_relaxed));
    }
    EXPECT_TRUE(pool->stopped());
    EXPECT_EQ(pool->pending_count(), 0U);
}

TEST(BoundedThreadPoolTest, SubmitShutdownRaceExecutesOnlyAcceptedWork) {
    auto pool = make_pool(2, 64);
    ASSERT_NE(pool, nullptr);
    constexpr std::size_t submission_count = 32;
    std::array<std::atomic<int>, submission_count> executions{};
    std::array<std::atomic<int>, submission_count> outcomes{};
    std::array<std::atomic<int>, submission_count> error_codes{};
    for (std::size_t index = 0; index < submission_count; ++index) {
        executions[index].store(0, std::memory_order_relaxed);
        outcomes[index].store(0, std::memory_order_relaxed);
        error_codes[index].store(0, std::memory_order_relaxed);
    }
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::vector<std::thread> submitters;
    submitters.reserve(submission_count);
    for (std::size_t index = 0; index < submission_count; ++index) {
        submitters.emplace_back([&, index] {
            gate.wait();
            auto submitted = pool->try_submit([&, index] {
                executions[index].fetch_add(1, std::memory_order_relaxed);
            });
            outcomes[index].store(
                submitted ? 1 : -1,
                std::memory_order_relaxed);
            if (!submitted) {
                error_codes[index].store(
                    static_cast<int>(submitted.error().code),
                    std::memory_order_relaxed);
            }
        });
    }
    std::atomic<bool> stopped{false};
    std::thread stopper{[&] {
        gate.wait();
        stopped.store(
            static_cast<bool>(pool->shutdown()),
            std::memory_order_relaxed);
    }};

    release.set_value();
    for (auto& submitter : submitters) {
        submitter.join();
    }
    stopper.join();

    EXPECT_TRUE(stopped.load(std::memory_order_relaxed));
    for (std::size_t index = 0; index < submission_count; ++index) {
        const int outcome = outcomes[index].load(std::memory_order_relaxed);
        ASSERT_NE(outcome, 0);
        EXPECT_EQ(
            executions[index].load(std::memory_order_relaxed),
            outcome == 1 ? 1 : 0);
        if (outcome == -1) {
            EXPECT_EQ(
                error_codes[index].load(std::memory_order_relaxed),
                static_cast<int>(ErrorCode::InvalidState));
        }
    }
    EXPECT_EQ(pool->pending_count(), 0U);
    EXPECT_TRUE(pool->stopped());
}

TEST(BoundedThreadPoolTest, RejectsSubmissionAfterShutdown) {
    auto pool = make_pool();
    ASSERT_NE(pool, nullptr);
    ASSERT_TRUE(pool->shutdown());

    auto submitted = pool->try_submit([] {});

    EXPECT_FALSE(submitted);
    EXPECT_EQ(submitted.error().code, ErrorCode::InvalidState);
}

TEST(BoundedThreadPoolTest, WorkerCannotShutDownItsOwnPool) {
    auto pool = make_pool();
    ASSERT_NE(pool, nullptr);
    std::promise<ErrorCode> observed;
    auto observed_future = observed.get_future();
    auto* const raw_pool = pool.get();

    ASSERT_TRUE(pool->try_submit([raw_pool, &observed] {
        const auto result = raw_pool->shutdown();
        observed.set_value(result ? ErrorCode::InternalError : result.error().code);
    }));

    ASSERT_EQ(observed_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(observed_future.get(), ErrorCode::InvalidState);
    EXPECT_TRUE(pool->accepting());
    EXPECT_TRUE(pool->shutdown());
    EXPECT_TRUE(pool->stopped());
}

TEST(BoundedThreadPoolTest, ExposesConfiguredWorkerCount) {
    auto pool = make_pool(3, 4);
    ASSERT_NE(pool, nullptr);

    EXPECT_EQ(pool->worker_count(), 3U);
    EXPECT_TRUE(pool->running());
    EXPECT_TRUE(pool->shutdown());
}

TEST(BoundedThreadPoolTest, DestructorDrainsAndJoinsWorkers) {
    std::atomic<int> executions{0};
    {
        auto pool = make_pool(2, 8);
        ASSERT_NE(pool, nullptr);
        for (int index = 0; index < 8; ++index) {
            ASSERT_TRUE(pool->try_submit([&executions] {
                executions.fetch_add(1, std::memory_order_relaxed);
            }));
        }
    }

    EXPECT_EQ(executions.load(std::memory_order_relaxed), 8);
}

TEST(BoundedThreadPoolTest, ExplicitShutdownLeavesNoJoinableWorkerObjects) {
    auto pool = make_pool(3, 4);
    ASSERT_NE(pool, nullptr);

    ASSERT_TRUE(pool->shutdown());
    ASSERT_TRUE(pool->stopped());
    ASSERT_EQ(pool->pending_count(), 0U);

    EXPECT_NO_FATAL_FAILURE(pool.reset());
}

}  // namespace
