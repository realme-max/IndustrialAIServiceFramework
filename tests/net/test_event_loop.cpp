#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "iaisf/core/error.hpp"
#include "iaisf/core/result.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/channel.hpp"
#include "iaisf/net/epoll_poller.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net {

struct EventLoopTestAccess {
    static int wakeup_fd(const EventLoop& loop) noexcept {
        return loop.wakeup_fd_.get();
    }

    static Result<void> disable_wakeup(EventLoop& loop) {
        if (loop.wakeup_channel_->is_registered()) {
            auto result = loop.poller_->remove(*loop.wakeup_channel_);
            if (!result) {
                return result;
            }
        }
        loop.wakeup_fd_.reset();
        return Result<void>::success();
    }

    static void dispatch(
        EventLoop& loop,
        const std::vector<Channel*>& active_channels) noexcept {
        loop.handle_active_channels(active_channels);
    }
};

}  // namespace iaisf::net

namespace {

using namespace std::chrono_literals;

class RecordingLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        const std::string_view message) override {
        std::lock_guard<std::mutex> lock{mutex_};
        messages_.emplace_back(message);
    }

    [[nodiscard]] bool contains(const std::string_view fragment) const {
        std::lock_guard<std::mutex> lock{mutex_};
        for (const auto& message : messages_) {
            if (message.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

class ThrowingLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        std::string_view) override {
        throw std::runtime_error{"logger failure"};
    }
};

class LoopThread final {
public:
    explicit LoopThread(
        RecordingLogger& logger,
        const std::size_t pending_capacity = 32U)
        : start_future_(start_promise_.get_future().share()),
          ready_future_(ready_promise_.get_future()),
          first_result_future_(first_result_promise_.get_future()),
          second_result_future_(second_result_promise_.get_future()),
          thread_([this, &logger, pending_capacity] {
              auto create_result =
                  iaisf::net::EventLoop::create(logger, 32U, pending_capacity);
              if (!create_result) {
                  const iaisf::Error error = create_result.error();
                  ready_promise_.set_value(nullptr);
                  first_result_promise_.set_value(
                      iaisf::Result<void>::failure(error));
                  second_result_promise_.set_value(
                      iaisf::Result<void>::failure(error));
                  return;
              }

              auto loop = std::move(create_result).value();
              loop_pointer_.store(loop.get(), std::memory_order_release);
              ready_promise_.set_value(loop.get());
              if (start_future_.wait_for(5s) != std::future_status::ready) {
                  const iaisf::Error error = iaisf::make_error(
                      iaisf::ErrorCode::InvalidState,
                      "LoopThread start signal timed out");
                  final_state_.store(loop->state(), std::memory_order_release);
                  first_result_promise_.set_value(
                      iaisf::Result<void>::failure(error));
                  second_result_promise_.set_value(
                      iaisf::Result<void>::failure(error));
                  loop_pointer_.store(nullptr, std::memory_order_release);
                  return;
              }

              auto first_result = loop->run();
              final_state_.store(loop->state(), std::memory_order_release);
              auto second_result = loop->run();
              first_result_promise_.set_value(std::move(first_result));
              second_result_promise_.set_value(std::move(second_result));
              loop_pointer_.store(nullptr, std::memory_order_release);
              loop.reset();
          }) {}

    LoopThread(const LoopThread&) = delete;
    LoopThread& operator=(const LoopThread&) = delete;

    ~LoopThread() {
        if (thread_.joinable()) {
            if (!ready_consumed_) {
                static_cast<void>(wait_until_ready());
            }
            if (auto* loop = loop_pointer_.load(std::memory_order_acquire)) {
                loop->stop();
            }
            start();
            thread_.join();
        }
    }

    iaisf::net::EventLoop* wait_until_ready() {
        if (ready_future_.wait_for(2s) != std::future_status::ready) {
            return nullptr;
        }
        if (!ready_consumed_) {
            cached_loop_ = ready_future_.get();
            ready_consumed_ = true;
        }
        return cached_loop_;
    }

    void start() {
        if (!started_) {
            started_ = true;
            start_promise_.set_value();
        }
    }

    bool wait_until_finished() {
        return first_result_future_.wait_for(2s) == std::future_status::ready;
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    iaisf::Result<void> first_result() {
        return first_result_future_.get();
    }

    iaisf::Result<void> second_result() {
        return second_result_future_.get();
    }

    iaisf::net::EventLoop::State final_state() const noexcept {
        return final_state_.load(std::memory_order_acquire);
    }

private:
    std::promise<void> start_promise_;
    std::shared_future<void> start_future_;
    std::promise<iaisf::net::EventLoop*> ready_promise_;
    std::future<iaisf::net::EventLoop*> ready_future_;
    std::promise<iaisf::Result<void>> first_result_promise_;
    std::future<iaisf::Result<void>> first_result_future_;
    std::promise<iaisf::Result<void>> second_result_promise_;
    std::future<iaisf::Result<void>> second_result_future_;
    std::atomic<iaisf::net::EventLoop*> loop_pointer_{nullptr};
    std::atomic<iaisf::net::EventLoop::State> final_state_{
        iaisf::net::EventLoop::State::Created};
    bool ready_consumed_{false};
    iaisf::net::EventLoop* cached_loop_{nullptr};
    bool started_{false};
    std::thread thread_;
};

TEST(EventLoopTest, RejectsInvalidPendingCallbackCapacity) {
    RecordingLogger logger;

    auto zero = iaisf::net::EventLoop::create(logger, 16U, 0U);
    auto excessive = iaisf::net::EventLoop::create(
        logger,
        16U,
        iaisf::net::EventLoop::kMaximumPendingCallbacks + 1U);

    ASSERT_FALSE(zero);
    ASSERT_FALSE(excessive);
    EXPECT_EQ(zero.error().code, iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(excessive.error().code, iaisf::ErrorCode::InvalidArgument);
}

TEST(EventLoopTest, StopBeforeRunIsIdempotentAndCancelsFutureExecution) {
    RecordingLogger logger;
    auto create_result = iaisf::net::EventLoop::create(logger, 16U, 4U);
    ASSERT_TRUE(create_result);
    auto loop = std::move(create_result).value();
    int execution_count = 0;
    ASSERT_TRUE(loop->queue_in_loop([&execution_count] {
        ++execution_count;
    }));

    loop->stop();
    loop->stop();
    auto rejected = loop->queue_in_loop([&execution_count] {
        ++execution_count;
    });
    auto run_result = loop->run();

    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, iaisf::ErrorCode::InvalidState);
    ASSERT_FALSE(run_result);
    EXPECT_EQ(run_result.error().code, iaisf::ErrorCode::InvalidState);
    EXPECT_EQ(execution_count, 0);
    EXPECT_EQ(loop->pending_callback_count(), 0U);
}

TEST(EventLoopTest, RejectsRunFromNonOwnerAndRejectsSecondRun) {
    RecordingLogger logger;
    LoopThread loop_thread{logger};
    auto* loop = loop_thread.wait_until_ready();
    ASSERT_NE(loop, nullptr);

    auto non_owner_result = loop->run();
    EXPECT_FALSE(non_owner_result);
    EXPECT_EQ(non_owner_result.error().code, iaisf::ErrorCode::InvalidState);

    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future();
    ASSERT_TRUE(loop->queue_in_loop([&entered_promise] {
        entered_promise.set_value();
    }));
    loop_thread.start();
    ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
    loop->stop();
    ASSERT_TRUE(loop_thread.wait_until_finished());
    loop_thread.join();
    auto first_result = loop_thread.first_result();
    auto second_result = loop_thread.second_result();
    EXPECT_TRUE(first_result);
    ASSERT_FALSE(second_result);
    EXPECT_EQ(second_result.error().code, iaisf::ErrorCode::InvalidState);
}

TEST(EventLoopTest, StopFromAnotherThreadWakesBlockedWait) {
    RecordingLogger logger;
    LoopThread loop_thread{logger};
    auto* loop = loop_thread.wait_until_ready();
    ASSERT_NE(loop, nullptr);
    std::mutex owner_check_mutex;
    std::condition_variable owner_check_condition;
    bool callback_ran_on_owner = false;
    bool owner_check_complete = false;
    ASSERT_TRUE(loop->queue_in_loop([
        loop,
        &owner_check_mutex,
        &owner_check_condition,
        &callback_ran_on_owner,
        &owner_check_complete] {
        {
            std::lock_guard<std::mutex> lock{owner_check_mutex};
            callback_ran_on_owner = loop->is_in_loop_thread();
            owner_check_complete = true;
        }
        owner_check_condition.notify_one();
    }));

    loop_thread.start();
    {
        std::unique_lock<std::mutex> lock{owner_check_mutex};
        ASSERT_TRUE(owner_check_condition.wait_for(
            lock,
            2s,
            [&owner_check_complete] { return owner_check_complete; }));
    }
    EXPECT_TRUE(callback_ran_on_owner);
    loop->stop();

    ASSERT_TRUE(loop_thread.wait_until_finished());
    loop_thread.join();
    EXPECT_TRUE(loop_thread.first_result());
    EXPECT_EQ(loop_thread.final_state(), iaisf::net::EventLoop::State::Stopped);
}

TEST(EventLoopTest, StoppingRejectsCallbacksAndRepeatedStopIsIdempotent) {
    RecordingLogger logger;
    LoopThread loop_thread{logger};
    auto* loop = loop_thread.wait_until_ready();
    ASSERT_NE(loop, nullptr);
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool callback_entered = false;
    bool release_callback = false;
    ASSERT_TRUE(loop->queue_in_loop([
        &gate_mutex,
        &gate_condition,
        &callback_entered,
        &release_callback] {
        std::unique_lock<std::mutex> lock{gate_mutex};
        callback_entered = true;
        gate_condition.notify_all();
        static_cast<void>(gate_condition.wait_for(
            lock,
            2s,
            [&release_callback] { return release_callback; }));
    }));

    loop_thread.start();
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock{gate_mutex};
        entered = gate_condition.wait_for(
            lock,
            2s,
            [&callback_entered] { return callback_entered; });
    }
    if (!entered) {
        {
            std::lock_guard<std::mutex> lock{gate_mutex};
            release_callback = true;
        }
        gate_condition.notify_all();
        loop->stop();
        static_cast<void>(loop_thread.wait_until_finished());
        loop_thread.join();
        ADD_FAILURE() << "owner callback did not start within the bounded wait";
        return;
    }

    loop->stop();
    loop->stop();
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopping);
    auto rejected = loop->queue_in_loop([] {});
    EXPECT_FALSE(rejected);
    if (!rejected) {
        EXPECT_EQ(rejected.error().code, iaisf::ErrorCode::InvalidState);
    }

    {
        std::lock_guard<std::mutex> lock{gate_mutex};
        release_callback = true;
    }
    gate_condition.notify_all();
    ASSERT_TRUE(loop_thread.wait_until_finished());
    loop_thread.join();
    EXPECT_TRUE(loop_thread.first_result());
    EXPECT_EQ(loop_thread.final_state(), iaisf::net::EventLoop::State::Stopped);
}

TEST(EventLoopTest, CrossThreadCallbacksExecuteExactlyOnce) {
    RecordingLogger logger;
    LoopThread loop_thread{logger};
    auto* loop = loop_thread.wait_until_ready();
    ASSERT_NE(loop, nullptr);
    constexpr int kCallbackCount = 8;
    std::atomic<int> executed{0};
    std::promise<void> completed_promise;
    auto completed = completed_promise.get_future();

    for (int index = 0; index < kCallbackCount; ++index) {
        ASSERT_TRUE(loop->queue_in_loop([loop, &executed, &completed_promise] {
            const int current = executed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (current == kCallbackCount) {
                completed_promise.set_value();
                loop->stop();
            }
        }));
    }

    loop_thread.start();
    ASSERT_EQ(completed.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(loop_thread.wait_until_finished());
    loop_thread.join();
    EXPECT_EQ(executed.load(std::memory_order_relaxed), kCallbackCount);
    EXPECT_TRUE(loop_thread.first_result());
}

TEST(EventLoopTest, ConcurrentProducersRespectCapacityAndExecuteAcceptedCallbacksOnce) {
    RecordingLogger logger;
    constexpr std::size_t kCapacity = 8U;
    constexpr std::size_t kProducerCount = 32U;
    LoopThread loop_thread{logger, kCapacity};
    auto* loop = loop_thread.wait_until_ready();
    ASSERT_NE(loop, nullptr);

    std::mutex start_mutex;
    std::condition_variable start_condition;
    std::size_t ready_count = 0U;
    bool release_producers = false;
    std::vector<int> accepted(kProducerCount, 0);
    std::vector<std::atomic<int>> executions(kProducerCount);
    for (auto& execution : executions) {
        execution.store(0, std::memory_order_relaxed);
    }
    std::atomic<int> executed_total{0};
    std::atomic<int> expected_successes{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);

    for (std::size_t index = 0U; index < kProducerCount; ++index) {
        producers.emplace_back([
            loop,
            index,
            &start_mutex,
            &start_condition,
            &ready_count,
            &release_producers,
            &accepted,
            &executions,
            &executed_total,
            &expected_successes] {
            {
                std::unique_lock<std::mutex> lock{start_mutex};
                ++ready_count;
                start_condition.notify_all();
                const bool released = start_condition.wait_for(
                    lock,
                    2s,
                    [&release_producers] { return release_producers; });
                if (!released) {
                    return;
                }
            }

            auto result = loop->queue_in_loop([
                loop,
                index,
                &executions,
                &executed_total,
                &expected_successes] {
                executions[index].fetch_add(1, std::memory_order_relaxed);
                const int total =
                    executed_total.fetch_add(1, std::memory_order_relaxed) + 1;
                if (total == expected_successes.load(std::memory_order_acquire)) {
                    loop->stop();
                }
            });
            accepted[index] = result ? 1 : 0;
        });
    }

    bool all_ready = false;
    {
        std::unique_lock<std::mutex> lock{start_mutex};
        all_ready = start_condition.wait_for(
            lock,
            2s,
            [&ready_count] { return ready_count == kProducerCount; });
        release_producers = true;
    }
    start_condition.notify_all();
    for (auto& producer : producers) {
        producer.join();
    }
    ASSERT_TRUE(all_ready);

    const int success_count =
        static_cast<int>(std::count(accepted.begin(), accepted.end(), 1));
    EXPECT_LE(success_count, static_cast<int>(kCapacity));
    ASSERT_GT(success_count, 0);
    expected_successes.store(success_count, std::memory_order_release);

    loop_thread.start();
    ASSERT_TRUE(loop_thread.wait_until_finished());
    loop_thread.join();
    EXPECT_TRUE(loop_thread.first_result());
    EXPECT_EQ(executed_total.load(std::memory_order_relaxed), success_count);

    for (std::size_t index = 0U; index < kProducerCount; ++index) {
        const int execution_count =
            executions[index].load(std::memory_order_relaxed);
        EXPECT_LE(execution_count, 1);
        EXPECT_EQ(execution_count, accepted[index]);
    }
}

TEST(EventLoopTest, CallbackCanQueueAnotherCallbackWithoutDeadlock) {
    RecordingLogger logger;
    LoopThread loop_thread{logger};
    auto* loop = loop_thread.wait_until_ready();
    ASSERT_NE(loop, nullptr);
    std::promise<bool> nested_result_promise;
    auto nested_result = nested_result_promise.get_future();
    std::promise<void> nested_executed_promise;
    auto nested_executed = nested_executed_promise.get_future();

    ASSERT_TRUE(loop->queue_in_loop(
        [loop, &nested_result_promise, &nested_executed_promise] {
            auto result = loop->queue_in_loop([loop, &nested_executed_promise] {
                nested_executed_promise.set_value();
                loop->stop();
            });
            nested_result_promise.set_value(static_cast<bool>(result));
        }));

    loop_thread.start();
    ASSERT_EQ(nested_result.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(nested_result.get());
    ASSERT_EQ(nested_executed.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(loop_thread.wait_until_finished());
    loop_thread.join();
    EXPECT_TRUE(loop_thread.first_result());
}

TEST(EventLoopTest, RejectsEmptyCallbackAndFullBoundedQueue) {
    RecordingLogger logger;
    auto create_result = iaisf::net::EventLoop::create(logger, 16U, 2U);
    ASSERT_TRUE(create_result);
    auto loop = std::move(create_result).value();
    int accepted_execution_count = 0;
    int rejected_execution_count = 0;

    auto empty_result = loop->queue_in_loop({});
    ASSERT_FALSE(empty_result);
    EXPECT_EQ(empty_result.error().code, iaisf::ErrorCode::InvalidArgument);
    ASSERT_TRUE(loop->queue_in_loop([&accepted_execution_count] {
        ++accepted_execution_count;
    }));
    ASSERT_TRUE(loop->queue_in_loop([&loop, &accepted_execution_count] {
        ++accepted_execution_count;
        loop->stop();
    }));

    auto full_result = loop->queue_in_loop([&rejected_execution_count] {
        ++rejected_execution_count;
    });
    ASSERT_FALSE(full_result);
    EXPECT_EQ(full_result.error().code, iaisf::ErrorCode::ResourceExhausted);
    EXPECT_EQ(loop->pending_callback_count(), 2U);

    auto run_result = loop->run();

    EXPECT_TRUE(run_result);
    EXPECT_EQ(accepted_execution_count, 2);
    EXPECT_EQ(rejected_execution_count, 0);
    EXPECT_EQ(loop->pending_callback_count(), 0U);
}

TEST(EventLoopTest, WakeupFailureRollsBackTheRejectedCallback) {
    RecordingLogger logger;
    auto create_result = iaisf::net::EventLoop::create(logger, 16U, 4U);
    ASSERT_TRUE(create_result);
    auto loop = std::move(create_result).value();
    ASSERT_TRUE(iaisf::net::EventLoopTestAccess::disable_wakeup(*loop));
    int execution_count = 0;

    auto result = loop->queue_in_loop([&execution_count] {
        ++execution_count;
    });

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::SystemError);
    EXPECT_EQ(loop->pending_callback_count(), 0U);
    EXPECT_EQ(execution_count, 0);
    loop->stop();
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
}

TEST(EventLoopTest, CallbackExceptionIsLoggedAndLaterCallbackStillRuns) {
    RecordingLogger logger;
    LoopThread loop_thread{logger};
    auto* loop = loop_thread.wait_until_ready();
    ASSERT_NE(loop, nullptr);
    std::promise<void> continued_promise;
    auto continued = continued_promise.get_future();

    ASSERT_TRUE(loop->queue_in_loop([] {
        throw std::runtime_error{"expected test exception"};
    }));
    ASSERT_TRUE(loop->queue_in_loop([] {
        throw 7;
    }));
    ASSERT_TRUE(loop->queue_in_loop([loop, &continued_promise] {
        continued_promise.set_value();
        loop->stop();
    }));

    loop_thread.start();
    ASSERT_EQ(continued.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(loop_thread.wait_until_finished());
    loop_thread.join();
    EXPECT_TRUE(loop_thread.first_result());
    EXPECT_TRUE(logger.contains("callback threw std::exception"));
    EXPECT_TRUE(logger.contains("callback threw an unknown exception"));
}

TEST(EventLoopTest, LoggerFailureDoesNotTerminateCallbackProcessing) {
    ThrowingLogger logger;
    auto create_result = iaisf::net::EventLoop::create(logger, 16U, 4U);
    ASSERT_TRUE(create_result);
    auto loop = std::move(create_result).value();
    bool later_callback_ran = false;
    ASSERT_TRUE(loop->queue_in_loop([] {
        throw std::runtime_error{"expected test exception"};
    }));
    ASSERT_TRUE(loop->queue_in_loop([&loop, &later_callback_ran] {
        later_callback_ran = true;
        loop->stop();
    }));

    auto run_result = loop->run();

    EXPECT_TRUE(run_result);
    EXPECT_TRUE(later_callback_ran);
    EXPECT_EQ(loop->logger_failure_count(), 1U);
}

TEST(EventLoopTest, ChannelExceptionStopsThatChannelButContinuesTheActiveBatch) {
    RecordingLogger logger;
    auto create_result = iaisf::net::EventLoop::create(logger, 16U, 4U);
    ASSERT_TRUE(create_result);
    auto loop = std::move(create_result).value();
    iaisf::net::Channel throwing_channel{*loop, -1};
    iaisf::net::Channel following_channel{*loop, -1};
    int skipped_callback_count = 0;
    int following_callback_count = 0;
    throwing_channel.set_error_callback([] {
        throw std::runtime_error{"expected channel exception"};
    });
    throwing_channel.set_read_callback([&skipped_callback_count] {
        ++skipped_callback_count;
    });
    following_channel.set_read_callback([&following_callback_count] {
        ++following_callback_count;
    });
    throwing_channel.set_ready_events(
        iaisf::net::Channel::kErrorEvent |
        static_cast<std::uint32_t>(EPOLLIN));
    following_channel.set_ready_events(
        static_cast<std::uint32_t>(EPOLLIN));
    const std::vector<iaisf::net::Channel*> active{
        &throwing_channel,
        &following_channel};

    iaisf::net::EventLoopTestAccess::dispatch(*loop, active);

    EXPECT_EQ(skipped_callback_count, 0);
    EXPECT_EQ(following_callback_count, 1);
    EXPECT_TRUE(logger.contains("Channel callback threw std::exception"));
}

TEST(EventLoopTest, ChannelRemovalIsDeferredUntilTheActiveBatchCompletes) {
    RecordingLogger logger;
    auto create_result = iaisf::net::EventLoop::create(logger, 16U, 8U);
    ASSERT_TRUE(create_result);
    auto loop = std::move(create_result).value();
    iaisf::net::UniqueFd first_fd{
        ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC)};
    iaisf::net::UniqueFd second_fd{
        ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC)};
    ASSERT_TRUE(first_fd.valid());
    ASSERT_TRUE(second_fd.valid());
    constexpr std::uint64_t signal_value = 1U;
    ASSERT_EQ(
        ::write(first_fd.get(), &signal_value, sizeof(signal_value)),
        static_cast<ssize_t>(sizeof(signal_value)));
    ASSERT_EQ(
        ::write(second_fd.get(), &signal_value, sizeof(signal_value)),
        static_cast<ssize_t>(sizeof(signal_value)));
    iaisf::net::Channel first_channel{*loop, first_fd.get()};
    iaisf::net::Channel second_channel{*loop, second_fd.get()};
    first_channel.enable_reading();
    second_channel.enable_reading();
    first_channel.set_edge_triggered(true);
    second_channel.set_edge_triggered(true);

    bool direct_removal_rejected = false;
    bool deferred_removal_succeeded = false;
    bool deferred_queue_succeeded = false;
    int first_callback_count = 0;
    int second_callback_count = 0;
    first_channel.set_read_callback([
        &loop,
        &first_channel,
        &second_channel,
        &direct_removal_rejected,
        &deferred_removal_succeeded,
        &deferred_queue_succeeded,
        &first_callback_count] {
        std::uint64_t value = 0U;
        static_cast<void>(::read(first_channel.fd(), &value, sizeof(value)));
        ++first_callback_count;

        auto direct_result = second_channel.remove();
        direct_removal_rejected =
            !direct_result &&
            direct_result.error().code == iaisf::ErrorCode::InvalidState;

        auto queued_result = loop->queue_in_loop([
            &loop,
            &second_channel,
            &deferred_removal_succeeded] {
            deferred_removal_succeeded =
                static_cast<bool>(second_channel.remove());
            loop->stop();
        });
        deferred_queue_succeeded = static_cast<bool>(queued_result);
        if (!queued_result) {
            loop->stop();
        }
    });
    second_channel.set_read_callback([
        &second_channel,
        &second_callback_count] {
        std::uint64_t value = 0U;
        static_cast<void>(::read(second_channel.fd(), &value, sizeof(value)));
        ++second_callback_count;
    });
    auto first_update = first_channel.update();
    if (!first_update) {
        ADD_FAILURE() << first_update.error().message;
        return;
    }
    auto second_update = second_channel.update();
    if (!second_update) {
        ADD_FAILURE() << second_update.error().message;
        EXPECT_TRUE(first_channel.remove());
        return;
    }

    auto run_result = loop->run();

    EXPECT_TRUE(run_result);
    EXPECT_TRUE(direct_removal_rejected);
    EXPECT_TRUE(deferred_queue_succeeded);
    EXPECT_TRUE(deferred_removal_succeeded);
    EXPECT_EQ(first_callback_count, 1);
    EXPECT_EQ(second_callback_count, 1);
    EXPECT_FALSE(second_channel.is_registered());
    if (first_channel.is_registered()) {
        EXPECT_TRUE(first_channel.remove());
    }
    if (second_channel.is_registered()) {
        EXPECT_TRUE(second_channel.remove());
    }
}

TEST(EventLoopTest, NonOwnerThreadCannotUpdateChannel) {
    RecordingLogger logger;
    LoopThread loop_thread{logger};
    auto* loop = loop_thread.wait_until_ready();
    ASSERT_NE(loop, nullptr);
    const int descriptor = ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(descriptor, 0);
    {
        iaisf::net::UniqueFd owner{descriptor};
        iaisf::net::Channel channel{*loop, descriptor};
        channel.enable_reading();

        auto result = channel.update();

        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, iaisf::ErrorCode::InvalidState);
    }
    loop->stop();
    loop_thread.start();
    ASSERT_TRUE(loop_thread.wait_until_finished());
    loop_thread.join();
}

TEST(EventLoopTest, StopPathFullyDrainsWakeupDescriptor) {
    RecordingLogger logger;
    auto create_result = iaisf::net::EventLoop::create(logger, 16U, 8U);
    ASSERT_TRUE(create_result);
    auto loop = std::move(create_result).value();
    ASSERT_TRUE(loop->queue_in_loop([] {}));
    ASSERT_TRUE(loop->queue_in_loop([] {}));
    ASSERT_TRUE(loop->queue_in_loop([&loop] { loop->stop(); }));

    auto run_result = loop->run();

    ASSERT_TRUE(run_result);
    std::uint64_t value = 0U;
    errno = 0;
    EXPECT_EQ(
        ::read(
            iaisf::net::EventLoopTestAccess::wakeup_fd(*loop),
            &value,
            sizeof(value)),
        -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
}

TEST(EventLoopTest, SaturatedEventfdCountsAsAnExistingWakeup) {
    RecordingLogger logger;
    auto create_result = iaisf::net::EventLoop::create(logger, 16U, 4U);
    ASSERT_TRUE(create_result);
    auto loop = std::move(create_result).value();
    const std::uint64_t saturated_value =
        std::numeric_limits<std::uint64_t>::max() - 1U;
    ASSERT_EQ(
        ::write(
            iaisf::net::EventLoopTestAccess::wakeup_fd(*loop),
            &saturated_value,
            sizeof(saturated_value)),
        static_cast<ssize_t>(sizeof(saturated_value)));
    int execution_count = 0;

    auto queue_result = loop->queue_in_loop([&loop, &execution_count] {
        ++execution_count;
        loop->stop();
    });
    ASSERT_TRUE(queue_result);
    auto run_result = loop->run();

    EXPECT_TRUE(run_result);
    EXPECT_EQ(execution_count, 1);
    std::uint64_t value = 0U;
    errno = 0;
    EXPECT_EQ(
        ::read(
            iaisf::net::EventLoopTestAccess::wakeup_fd(*loop),
            &value,
            sizeof(value)),
        -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
}

}  // namespace
