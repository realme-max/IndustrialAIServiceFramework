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
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "iaisf/core/error.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/timer.hpp"
#include "timer_queue.hpp"

namespace {

using iaisf::ErrorCode;
using iaisf::net::TimerCallback;
using iaisf::net::TimerCancelOutcome;
using iaisf::net::TimerId;
using iaisf::net::TimerQueueOptions;
using iaisf::net::detail::TimerQueue;
using iaisf::net::detail::TimerQueueTestAccess;

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
        throw std::runtime_error{"expected logger failure"};
    }
};

struct QueueStorage final {
    RecordingLogger logger;
    std::size_t notification_count{0U};
    std::unique_ptr<iaisf::net::EventLoop> loop;
    std::unique_ptr<TimerQueue> queue;
};

class QueueHandle final {
public:
    explicit QueueHandle(std::unique_ptr<QueueStorage> storage) noexcept
        : storage_(std::move(storage)) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return storage_ && storage_->queue;
    }

    [[nodiscard]] TimerQueue* operator->() const noexcept {
        return storage_->queue.get();
    }

    [[nodiscard]] TimerQueue& operator*() const noexcept {
        return *storage_->queue;
    }

private:
    std::unique_ptr<QueueStorage> storage_;
};

QueueHandle make_queue(
    const std::size_t max_timers = TimerQueueOptions::kDefaultMaxTimers) {
    auto storage = std::make_unique<QueueStorage>();
    auto loop_result = iaisf::net::EventLoop::create(storage->logger, 32U, 32U);
    EXPECT_TRUE(loop_result);
    if (!loop_result) {
        return QueueHandle{nullptr};
    }
    storage->loop = std::move(loop_result).value();

    auto options_result = TimerQueueOptions::create(max_timers);
    EXPECT_TRUE(options_result);
    if (!options_result) {
        return QueueHandle{nullptr};
    }
    QueueStorage* const storage_pointer = storage.get();
    auto queue_result = TimerQueue::create(
        *storage->loop,
        options_result.value(),
        [storage_pointer](const char*, bool) {
            ++storage_pointer->notification_count;
        });
    EXPECT_TRUE(queue_result);
    if (!queue_result) {
        return QueueHandle{nullptr};
    }
    storage->queue = std::move(queue_result).value();
    return QueueHandle{std::move(storage)};
}

std::unique_ptr<iaisf::net::EventLoop> make_event_loop(
    RecordingLogger& logger) {
    auto result = iaisf::net::EventLoop::create(logger, 32U, 32U);
    EXPECT_TRUE(result);
    if (!result) {
        return nullptr;
    }
    return std::move(result).value();
}

TEST(TimerIdTest, DefaultConstructedIdIsInvalid) {
    const TimerId id;

    EXPECT_FALSE(id.valid());
    EXPECT_EQ(id, TimerId{});
}

TEST(TimerQueueOptionsTest, UsesBoundedDefaultCapacity) {
    const TimerQueueOptions options;

    EXPECT_EQ(options.max_timers, 1024U);
    EXPECT_EQ(TimerQueueOptions::kDefaultMaxTimers, 1024U);
    EXPECT_EQ(TimerQueueOptions::kHardMaxTimers, 1'000'000U);
}

TEST(TimerQueueOptionsTest, RejectsZeroCapacity) {
    auto result = TimerQueueOptions::create(0U);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerQueueOptionsTest, AcceptsHardMaximumCapacity) {
    auto result = TimerQueueOptions::create(
        TimerQueueOptions::kHardMaxTimers);

    ASSERT_TRUE(result);
    EXPECT_EQ(
        result.value().max_timers,
        TimerQueueOptions::kHardMaxTimers);
}

TEST(TimerQueueOptionsTest, RejectsCapacityAboveHardMaximum) {
    auto result = TimerQueueOptions::create(
        TimerQueueOptions::kHardMaxTimers + 1U);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerQueueTest, RejectsMutatedInvalidOptionsAtQueueCreation) {
    TimerQueueOptions options;
    options.max_timers = 0U;
    RecordingLogger logger;
    auto loop_result = iaisf::net::EventLoop::create(logger, 16U, 16U);
    ASSERT_TRUE(loop_result);
    auto loop = std::move(loop_result).value();

    auto result = TimerQueue::create(
        *loop,
        options,
        [](const char*, bool) noexcept {
            static_cast<void>(0);
        });

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerQueueTest, AllocatesUniqueMonotonicIdsWithoutReuse) {
    auto queue = make_queue(3U);
    ASSERT_TRUE(queue);
    const auto deadline = TimerQueue::TimePoint{};

    auto first = queue->run_at(deadline, [] {});
    auto second = queue->run_at(deadline, [] {});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.value(), second.value());
    EXPECT_LT(
        TimerQueueTestAccess::sequence(first.value()),
        TimerQueueTestAccess::sequence(second.value()));
    ASSERT_TRUE(queue->cancel(first.value()));

    auto third = queue->run_at(deadline, [] {});
    ASSERT_TRUE(third);
    EXPECT_NE(third.value(), first.value());
    EXPECT_GT(
        TimerQueueTestAccess::sequence(third.value()),
        TimerQueueTestAccess::sequence(second.value()));
}

TEST(TimerQueueTest, OrdersTimersByDeadline) {
    auto queue = make_queue(3U);
    ASSERT_TRUE(queue);
    const auto origin = TimerQueue::TimePoint{};

    auto later = queue->run_at(origin + std::chrono::seconds{3}, [] {});
    auto first = queue->run_at(origin + std::chrono::seconds{1}, [] {});
    auto middle = queue->run_at(origin + std::chrono::seconds{2}, [] {});
    ASSERT_TRUE(later);
    ASSERT_TRUE(first);
    ASSERT_TRUE(middle);

    const std::vector<TimerId> expected{
        first.value(),
        middle.value(),
        later.value()};
    EXPECT_EQ(TimerQueueTestAccess::scheduled_ids(*queue), expected);
}

TEST(TimerQueueTest, UsesSequenceOrderForEqualDeadlines) {
    auto queue = make_queue(3U);
    ASSERT_TRUE(queue);
    const auto deadline = TimerQueue::TimePoint{};

    auto first = queue->run_at(deadline, [] {});
    auto second = queue->run_at(deadline, [] {});
    auto third = queue->run_at(deadline, [] {});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);

    const std::vector<TimerId> expected{
        first.value(),
        second.value(),
        third.value()};
    EXPECT_EQ(TimerQueueTestAccess::scheduled_ids(*queue), expected);
}

TEST(TimerQueueTest, EnforcesCapacityWithoutLeavingResidualState) {
    auto queue = make_queue(2U);
    ASSERT_TRUE(queue);

    ASSERT_TRUE(queue->run_at(TimerQueue::TimePoint{}, [] {}));
    ASSERT_TRUE(queue->run_at(TimerQueue::TimePoint{}, [] {}));
    auto rejected = queue->run_at(TimerQueue::TimePoint{}, [] {});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(TimerQueueTestAccess::timer_count(*queue), 2U);
    EXPECT_EQ(TimerQueueTestAccess::scheduled_ids(*queue).size(), 2U);
}

TEST(TimerQueueTest, RejectsEmptyCallbackWithoutConsumingId) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);

    auto rejected = queue->run_at(
        TimerQueue::TimePoint{},
        TimerCallback{});
    auto accepted = queue->run_at(TimerQueue::TimePoint{}, [] {});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(TimerQueueTestAccess::sequence(accepted.value()), 1U);
}

TEST(TimerQueueTest, RejectsNegativeDelayWithoutConsumingId) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);

    auto rejected = queue->run_after(-std::chrono::nanoseconds{1}, [] {});
    auto accepted = queue->run_after(TimerQueue::Duration::zero(), [] {});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(TimerQueueTestAccess::sequence(accepted.value()), 1U);
}

TEST(TimerQueueTest, CallbackCopyFailureLeavesQueueUnchanged) {
    struct ThrowOnCopy final {
        ThrowOnCopy() = default;
        ThrowOnCopy(const ThrowOnCopy&) {
            throw std::runtime_error{"expected copy failure"};
        }
        ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
        ThrowOnCopy& operator=(const ThrowOnCopy&) = delete;
        void operator()() const {}
    };

    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    TimerCallback callback{ThrowOnCopy{}};

    auto result = queue->run_at(TimerQueue::TimePoint{}, callback);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InternalError);
    EXPECT_EQ(TimerQueueTestAccess::timer_count(*queue), 0U);
    auto accepted = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(accepted);
    EXPECT_EQ(TimerQueueTestAccess::sequence(accepted.value()), 1U);
}

TEST(TimerQueueTest, ScheduleInsertionFailureRollsBackAndPreservesId) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    TimerQueueTestAccess::fail_next_add(
        *queue,
        TimerQueueTestAccess::AddFailurePoint::BeforeScheduleInsert);

    auto rejected = queue->run_at(TimerQueue::TimePoint{}, [] {});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(TimerQueueTestAccess::timer_count(*queue), 0U);
    EXPECT_TRUE(TimerQueueTestAccess::scheduled_ids(*queue).empty());
    auto accepted = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(accepted);
    EXPECT_EQ(TimerQueueTestAccess::sequence(accepted.value()), 1U);
}

TEST(TimerQueueTest, IdIndexInsertionFailureRollsBackScheduleAndPreservesId) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    TimerQueueTestAccess::fail_next_add(
        *queue,
        TimerQueueTestAccess::AddFailurePoint::BeforeIdIndexInsert);

    auto rejected = queue->run_at(TimerQueue::TimePoint{}, [] {});

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(TimerQueueTestAccess::timer_count(*queue), 0U);
    EXPECT_TRUE(TimerQueueTestAccess::scheduled_ids(*queue).empty());
    auto accepted = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(accepted);
    EXPECT_EQ(TimerQueueTestAccess::sequence(accepted.value()), 1U);
}

TEST(TimerQueueTest, AllocatesMaximumIdOnceThenReportsPermanentExhaustion) {
    auto queue = make_queue(2U);
    ASSERT_TRUE(queue);
    TimerQueueTestAccess::set_next_sequence(
        *queue,
        std::numeric_limits<std::uint64_t>::max());

    auto maximum = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(maximum);
    EXPECT_TRUE(maximum.value().valid());
    EXPECT_EQ(
        TimerQueueTestAccess::sequence(maximum.value()),
        std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(queue->cancel(maximum.value()));

    auto exhausted = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(TimerQueueTestAccess::timer_count(*queue), 0U);
}

TEST(TimerQueueTest, FailureAtMaximumIdDoesNotExhaustSequence) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    TimerQueueTestAccess::set_next_sequence(
        *queue,
        std::numeric_limits<std::uint64_t>::max());
    TimerQueueTestAccess::fail_next_add(
        *queue,
        TimerQueueTestAccess::AddFailurePoint::BeforeIdIndexInsert);

    auto rejected = queue->run_at(TimerQueue::TimePoint{}, [] {});
    auto accepted = queue->run_at(TimerQueue::TimePoint{}, [] {});

    ASSERT_FALSE(rejected);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(
        TimerQueueTestAccess::sequence(accepted.value()),
        std::numeric_limits<std::uint64_t>::max());
}

TEST(TimerQueueTest, CancelsScheduledTimerFromBothIndexes) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    auto added = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(added);

    auto cancelled = queue->cancel(added.value());

    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled.value(), TimerCancelOutcome::Cancelled);
    EXPECT_FALSE(TimerQueueTestAccess::contains(*queue, added.value()));
    EXPECT_EQ(TimerQueueTestAccess::timer_count(*queue), 0U);
    EXPECT_TRUE(TimerQueueTestAccess::scheduled_ids(*queue).empty());
}

TEST(TimerQueueTest, CancellingSameTimerTwiceReturnsNotFound) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    auto added = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(added);
    ASSERT_TRUE(queue->cancel(added.value()));

    auto second = queue->cancel(added.value());

    ASSERT_TRUE(second);
    EXPECT_EQ(second.value(), TimerCancelOutcome::NotFound);
}

TEST(TimerQueueTest, RejectsInvalidTimerId) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);

    auto result = queue->cancel(TimerId{});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerQueueTest, CancelsPendingDispatchBeforeCallbackStarts) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    auto added = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(added);
    TimerQueueTestAccess::make_pending_dispatch(*queue, added.value());

    auto result = queue->cancel(added.value());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), TimerCancelOutcome::Cancelled);
    EXPECT_EQ(TimerQueueTestAccess::timer_count(*queue), 0U);
}

TEST(TimerQueueTest, DispatchingOneShotIsTooLateToCancel) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    auto added = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(added);
    TimerQueueTestAccess::make_dispatching(*queue, added.value(), false);

    auto result = queue->cancel(added.value());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), TimerCancelOutcome::TooLate);
    EXPECT_TRUE(TimerQueueTestAccess::contains(*queue, added.value()));
    EXPECT_FALSE(
        TimerQueueTestAccess::cancel_requested(*queue, added.value()));
}

TEST(TimerQueueTest, DispatchingRepeatingLatchesCancelAndReturnsTooLate) {
    auto queue = make_queue(1U);
    ASSERT_TRUE(queue);
    auto added = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(added);
    TimerQueueTestAccess::make_dispatching(*queue, added.value(), true);

    auto result = queue->cancel(added.value());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), TimerCancelOutcome::TooLate);
    EXPECT_TRUE(
        TimerQueueTestAccess::cancel_requested(*queue, added.value()));
}

TEST(TimerFdIntegrationTest, CreatesNonblockingCloseOnExecDescriptorAndClosesIt) {
    RecordingLogger logger;
    int descriptor = -1;
    {
        auto loop = make_event_loop(logger);
        ASSERT_NE(loop, nullptr);
        auto& queue = TimerQueueTestAccess::queue(*loop);
        descriptor = TimerQueueTestAccess::timer_fd(queue);
        ASSERT_GE(descriptor, 0);

        const int status_flags = ::fcntl(descriptor, F_GETFL);
        const int descriptor_flags = ::fcntl(descriptor, F_GETFD);
        ASSERT_GE(status_flags, 0);
        ASSERT_GE(descriptor_flags, 0);
        EXPECT_NE(status_flags & O_NONBLOCK, 0);
        EXPECT_NE(descriptor_flags & FD_CLOEXEC, 0);
    }

    errno = 0;
    EXPECT_EQ(::fcntl(descriptor, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(TimerFdIntegrationTest, RunAfterDispatchesOneShotExactlyOnce) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    int callback_count = 0;

    auto scheduled = loop->run_after(0ns, [&loop, &callback_count] {
        ++callback_count;
        loop->stop();
    });
    ASSERT_TRUE(scheduled);

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
}

TEST(TimerFdIntegrationTest, CallbackDoesNotRunBeforeItsDeadline) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    constexpr auto delay = 10ms;
    const auto start = TimerQueue::Clock::now();
    TimerQueue::TimePoint callback_time{};

    ASSERT_TRUE(loop->run_after(delay, [&loop, &callback_time] {
        callback_time = TimerQueue::Clock::now();
        loop->stop();
    }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_GE(callback_time, start + delay);
}

TEST(TimerFdIntegrationTest, DispatchesMultipleTimersByDeadline) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    const auto base = TimerQueue::Clock::now();
    std::vector<int> order;

    ASSERT_TRUE(queue.run_at(base + 20ms, [&loop, &order] {
        order.push_back(3);
        loop->stop();
    }));
    ASSERT_TRUE(queue.run_at(base, [&order] { order.push_back(1); }));
    ASSERT_TRUE(queue.run_at(base + 10ms, [&order] { order.push_back(2); }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(TimerFdIntegrationTest, EqualDeadlinesUseSequenceOrder) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    const auto deadline = TimerQueue::Clock::now() + 1ms;
    std::vector<int> order;

    ASSERT_TRUE(queue.run_at(deadline, [&order] { order.push_back(1); }));
    ASSERT_TRUE(queue.run_at(deadline, [&order] { order.push_back(2); }));
    ASSERT_TRUE(queue.run_at(deadline, [&loop, &order] {
        order.push_back(3);
        loop->stop();
    }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
}

TEST(TimerFdIntegrationTest, CancelBeforeFirePreventsCallback) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    int cancelled_callback_count = 0;
    auto scheduled = loop->run_after(1h, [&cancelled_callback_count] {
        ++cancelled_callback_count;
    });
    ASSERT_TRUE(scheduled);

    auto cancelled = loop->cancel_timer(scheduled.value());
    ASSERT_TRUE(cancelled);
    EXPECT_EQ(cancelled.value(), TimerCancelOutcome::Cancelled);
    ASSERT_TRUE(loop->run_after(0ns, [&loop] { loop->stop(); }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(cancelled_callback_count, 0);
}

TEST(TimerFdIntegrationTest, CancelUnknownAndInvalidIdsHaveStableOutcomes) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto scheduled = loop->run_after(1h, [] {});
    ASSERT_TRUE(scheduled);
    ASSERT_TRUE(loop->cancel_timer(scheduled.value()));

    auto unknown = loop->cancel_timer(scheduled.value());
    auto invalid = loop->cancel_timer(TimerId{});

    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown.value(), TimerCancelOutcome::NotFound);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerFdIntegrationTest, CallbackCanScheduleTimerOutsideCurrentSnapshot) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    std::vector<int> order;
    bool nested_schedule_succeeded = false;

    ASSERT_TRUE(loop->run_after(
        0ns,
        [&loop, &order, &nested_schedule_succeeded] {
            order.push_back(1);
            auto nested = loop->run_after(0ns, [&loop, &order] {
                order.push_back(2);
                loop->stop();
            });
            nested_schedule_succeeded = static_cast<bool>(nested);
        }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_TRUE(nested_schedule_succeeded);
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
}

TEST(TimerFdIntegrationTest, CallbackCanCancelPendingSnapshotTimer) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    const auto deadline = TimerQueue::Clock::now() + 1ms;
    TimerId second_id;
    TimerCancelOutcome cancel_outcome = TimerCancelOutcome::NotFound;
    int second_callback_count = 0;

    ASSERT_TRUE(queue.run_at(
        deadline,
        [&loop, &second_id, &cancel_outcome] {
            auto cancelled = loop->cancel_timer(second_id);
            if (cancelled) {
                cancel_outcome = cancelled.value();
            }
            auto stopper = loop->run_after(0ns, [&loop] { loop->stop(); });
            EXPECT_TRUE(stopper);
        }));
    auto second = queue.run_at(deadline, [&second_callback_count] {
        ++second_callback_count;
    });
    ASSERT_TRUE(second);
    second_id = second.value();

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(cancel_outcome, TimerCancelOutcome::Cancelled);
    EXPECT_EQ(second_callback_count, 0);
}

TEST(TimerFdIntegrationTest, TimerCallbackCanStopWithoutRemovingActiveChannel) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    bool callback_saw_active_batch = false;

    ASSERT_TRUE(loop->run_after(0ns, [&loop, &callback_saw_active_batch] {
        callback_saw_active_batch = loop->dispatching_active_channels();
        loop->stop();
    }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_TRUE(callback_saw_active_batch);
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
}

TEST(TimerFdIntegrationTest, StopBeforeRunPreventsScheduledCallback) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    int callback_count = 0;
    ASSERT_TRUE(loop->run_after(0ns, [&callback_count] { ++callback_count; }));

    loop->stop();
    auto result = loop->run();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(callback_count, 0);
}

TEST(TimerFdIntegrationTest, CallbackExceptionDoesNotSkipLaterTimer) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    const auto deadline = TimerQueue::Clock::now() + 1ms;
    int later_callback_count = 0;

    ASSERT_TRUE(queue.run_at(deadline, [] {
        throw std::runtime_error{"expected timer callback failure"};
    }));
    ASSERT_TRUE(queue.run_at(deadline, [&loop, &later_callback_count] {
        ++later_callback_count;
        loop->stop();
    }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(later_callback_count, 1);
    EXPECT_TRUE(logger.contains("timer callback threw std::exception"));
}

TEST(TimerFdIntegrationTest, StopSkipsRemainingExpiredSnapshotCallbacks) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    const auto deadline = TimerQueue::Clock::now() + 1ms;
    int skipped_callback_count = 0;

    ASSERT_TRUE(queue.run_at(deadline, [&loop] { loop->stop(); }));
    ASSERT_TRUE(queue.run_at(deadline, [&skipped_callback_count] {
        ++skipped_callback_count;
    }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(skipped_callback_count, 0);
}

TEST(TimerFdIntegrationTest, ArmFailureRollsBackAndFailsClosed) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    TimerQueueTestAccess::fail_next_arm(queue);
    int callback_count = 0;

    auto result = loop->run_after(0ns, [&callback_count] { ++callback_count; });

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::SystemError);
    EXPECT_TRUE(TimerQueueTestAccess::faulted(queue));
    EXPECT_EQ(TimerQueueTestAccess::timer_count(queue), 0U);
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
    EXPECT_EQ(callback_count, 0);
}

TEST(TimerFdIntegrationTest, ReadFailureStopsWithoutExecutingCallback) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    TimerQueueTestAccess::fail_next_read(queue);
    int callback_count = 0;
    ASSERT_TRUE(loop->run_after(0ns, [&callback_count] { ++callback_count; }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_TRUE(TimerQueueTestAccess::faulted(queue));
    EXPECT_EQ(callback_count, 0);
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
    EXPECT_TRUE(logger.contains("failed to drain timerfd"));
}

TEST(TimerFdIntegrationTest, NonOwnerThreadCannotScheduleTimer) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    std::promise<iaisf::Result<TimerId>> result_promise;
    auto result_future = result_promise.get_future();

    std::thread caller{[&loop, &result_promise] {
        result_promise.set_value(loop->run_after(0ns, [] {}));
    }};
    caller.join();
    auto result = result_future.get();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
}

TEST(TimerFdIntegrationTest, RunEveryDispatchesRepeatingTimer) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    int callback_count = 0;

    auto scheduled = loop->run_every(1ms, [&loop, &callback_count] {
        ++callback_count;
        loop->stop();
    });
    ASSERT_TRUE(scheduled);

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
}

TEST(TimerFdIntegrationTest, RunEveryRejectsZeroInterval) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);

    auto result = loop->run_every(0ns, [] {});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerFdIntegrationTest, RunEveryRejectsNegativeInterval) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);

    auto result = loop->run_every(-1ns, [] {});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerFdIntegrationTest, RunEveryRejectsEmptyCallback) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);

    auto result = loop->run_every(1ms, TimerCallback{});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerFdIntegrationTest, RunEveryRejectsInitialDeadlineOverflow) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);

    auto result = loop->run_every(TimerQueue::Duration::max(), [] {});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerFdIntegrationTest, RepeatingTimerRunsAcrossMultiplePeriods) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    int callback_count = 0;
    TimerId repeating_id;
    TimerCancelOutcome self_cancel = TimerCancelOutcome::NotFound;

    auto scheduled = loop->run_every(
        1ms,
        [&loop, &callback_count, &repeating_id, &self_cancel] {
            ++callback_count;
            if (callback_count == 3) {
                auto cancelled = loop->cancel_timer(repeating_id);
                if (cancelled) {
                    self_cancel = cancelled.value();
                }
                loop->stop();
            }
        });
    ASSERT_TRUE(scheduled);
    repeating_id = scheduled.value();

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 3);
    EXPECT_EQ(self_cancel, TimerCancelOutcome::TooLate);
}

TEST(TimerQueueTest, RepeatingDeadlineUsesFixedRatePhase) {
    const TimerQueue::TimePoint previous{100ns};

    auto result = TimerQueueTestAccess::next_repeating_deadline(
        previous,
        10ns,
        previous + 4ns);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), previous + 10ns);
}

TEST(TimerQueueTest, RepeatingDeadlineSkipsMissedIntervals) {
    const TimerQueue::TimePoint previous{100ns};

    auto result = TimerQueueTestAccess::next_repeating_deadline(
        previous,
        10ns,
        previous + 35ns);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), previous + 40ns);
}

TEST(TimerFdIntegrationTest, MissedIntervalsDoNotReplayCallbacksInBurst) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    std::mutex wait_mutex;
    std::condition_variable wait_condition;
    int callback_count = 0;
    int marker_count = 0;
    TimerId repeating_id;

    auto scheduled = loop->run_every(
        1ms,
        [&loop,
         &wait_mutex,
         &wait_condition,
         &callback_count,
         &marker_count,
         &repeating_id] {
            ++callback_count;
            std::unique_lock<std::mutex> lock{wait_mutex};
            static_cast<void>(wait_condition.wait_for(lock, 10ms));
            lock.unlock();
            auto marker = loop->run_after(
                0ns,
                [&loop, &marker_count, &repeating_id] {
                    ++marker_count;
                    auto cancelled = loop->cancel_timer(repeating_id);
                    EXPECT_TRUE(cancelled);
                    loop->stop();
                });
            EXPECT_TRUE(marker);
        });
    ASSERT_TRUE(scheduled);
    repeating_id = scheduled.value();

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(marker_count, 1);
}

TEST(TimerFdIntegrationTest, RepeatingTimerCanCancelItself) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    TimerId repeating_id;
    TimerCancelOutcome outcome = TimerCancelOutcome::NotFound;
    int callback_count = 0;

    auto scheduled = loop->run_every(
        1ms,
        [&loop, &repeating_id, &outcome, &callback_count] {
            ++callback_count;
            auto cancelled = loop->cancel_timer(repeating_id);
            if (cancelled) {
                outcome = cancelled.value();
            }
            auto stopper = loop->run_after(1ms, [&loop] { loop->stop(); });
            EXPECT_TRUE(stopper);
        });
    ASSERT_TRUE(scheduled);
    repeating_id = scheduled.value();

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(outcome, TimerCancelOutcome::TooLate);
    EXPECT_EQ(callback_count, 1);
}

TEST(TimerFdIntegrationTest, CallbackCanCancelAnotherRepeatingTimer) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    int cancelled_callback_count = 0;
    TimerCancelOutcome outcome = TimerCancelOutcome::NotFound;
    auto other = loop->run_every(1h, [&cancelled_callback_count] {
        ++cancelled_callback_count;
    });
    ASSERT_TRUE(other);

    ASSERT_TRUE(loop->run_every(1ms, [&loop, &other, &outcome] {
        auto cancelled = loop->cancel_timer(other.value());
        if (cancelled) {
            outcome = cancelled.value();
        }
        loop->stop();
    }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(outcome, TimerCancelOutcome::Cancelled);
    EXPECT_EQ(cancelled_callback_count, 0);
}

TEST(TimerFdIntegrationTest, RepeatingTimerCanBeCancelledBeforeNextFire) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    TimerId repeating_id;
    TimerCancelOutcome outcome = TimerCancelOutcome::NotFound;
    int callback_count = 0;

    auto scheduled = loop->run_every(
        2ms,
        [&loop, &repeating_id, &callback_count, &outcome] {
            ++callback_count;
            auto canceller = loop->run_after(
                0ns,
                [&loop, &repeating_id, &outcome] {
                    auto cancelled = loop->cancel_timer(repeating_id);
                    if (cancelled) {
                        outcome = cancelled.value();
                    }
                    loop->stop();
                });
            EXPECT_TRUE(canceller);
        });
    ASSERT_TRUE(scheduled);
    repeating_id = scheduled.value();

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(outcome, TimerCancelOutcome::Cancelled);
}

TEST(TimerFdIntegrationTest, StandardExceptionRemovesRepeatingTimer) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    int callback_count = 0;

    ASSERT_TRUE(loop->run_every(1ms, [&callback_count] {
        ++callback_count;
        throw std::runtime_error{"expected repeating timer failure"};
    }));
    ASSERT_TRUE(loop->run_after(5ms, [&loop] { loop->stop(); }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_TRUE(logger.contains("timer callback threw std::exception"));
}

TEST(TimerFdIntegrationTest, UnknownExceptionRemovesRepeatingTimer) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    int callback_count = 0;

    ASSERT_TRUE(loop->run_every(1ms, [&callback_count] {
        ++callback_count;
        throw 7;
    }));
    ASSERT_TRUE(loop->run_after(5ms, [&loop] { loop->stop(); }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_TRUE(logger.contains("timer callback threw an unknown exception"));
}

TEST(TimerFdIntegrationTest, LoggerFailureDoesNotBreakRepeatingCleanup) {
    ThrowingLogger logger;
    auto loop_result = iaisf::net::EventLoop::create(logger, 32U, 32U);
    ASSERT_TRUE(loop_result);
    auto loop = std::move(loop_result).value();
    int callback_count = 0;

    ASSERT_TRUE(loop->run_every(1ms, [&callback_count] {
        ++callback_count;
        throw std::runtime_error{"expected repeating timer failure"};
    }));
    ASSERT_TRUE(loop->run_after(5ms, [&loop] { loop->stop(); }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(loop->logger_failure_count(), 1U);
}

TEST(TimerFdIntegrationTest, RepeatingCallbackStopPreventsReinsert) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    int callback_count = 0;

    ASSERT_TRUE(loop->run_every(1ms, [&loop, &callback_count] {
        ++callback_count;
        loop->stop();
    }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(TimerQueueTestAccess::timer_count(queue), 0U);
    EXPECT_TRUE(TimerQueueTestAccess::scheduled_ids(queue).empty());
}

TEST(TimerQueueTest, ShutdownRejectsNewRepeatingTimer) {
    auto queue = make_queue();
    ASSERT_TRUE(queue);
    ASSERT_TRUE(queue->shutdown());

    auto result = queue->run_every(1ms, [] {});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
}

TEST(TimerFdIntegrationTest, RepeatingRearmFailureFaultsAndStopsLoop) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    int callback_count = 0;

    ASSERT_TRUE(loop->run_every(1ms, [&queue, &callback_count] {
        ++callback_count;
        TimerQueueTestAccess::fail_next_arm(queue);
    }));

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_TRUE(TimerQueueTestAccess::faulted(queue));
    EXPECT_EQ(TimerQueueTestAccess::timer_count(queue), 0U);
    EXPECT_TRUE(logger.contains("failed to rearm repeating timer"));
}

TEST(TimerQueueTest, RepeatingDeadlineOverflowFailsClosedCalculation) {
    const TimerQueue::TimePoint previous = TimerQueue::TimePoint::max() - 1ns;

    auto result = TimerQueueTestAccess::next_repeating_deadline(
        previous,
        2ns,
        previous);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
}

TEST(TimerFdIntegrationTest, RepeatingDeadlineOverflowFaultsAndStopsLoop) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto& queue = TimerQueueTestAccess::queue(*loop);
    int callback_count = 0;
    auto scheduled = loop->run_every(1ms, [&callback_count] {
        ++callback_count;
    });
    ASSERT_TRUE(scheduled);
    TimerQueueTestAccess::set_record_deadline(
        queue,
        scheduled.value(),
        TimerQueue::TimePoint::max() - 1ns);

    auto result = loop->run();

    EXPECT_TRUE(result);
    EXPECT_EQ(callback_count, 1);
    EXPECT_TRUE(TimerQueueTestAccess::faulted(queue));
    EXPECT_EQ(TimerQueueTestAccess::timer_count(queue), 0U);
    EXPECT_TRUE(logger.contains("repeating timer deadline overflowed"));
}

TEST(TimerFdIntegrationTest, NonOwnerThreadCannotScheduleRepeatingTimer) {
    RecordingLogger logger;
    auto loop = make_event_loop(logger);
    ASSERT_NE(loop, nullptr);
    std::promise<iaisf::Result<TimerId>> result_promise;
    auto result_future = result_promise.get_future();

    std::thread caller{[&loop, &result_promise] {
        result_promise.set_value(loop->run_every(1ms, [] {}));
    }};
    caller.join();
    auto result = result_future.get();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
}

}  // namespace
