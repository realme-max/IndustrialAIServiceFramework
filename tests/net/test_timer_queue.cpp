#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "iaisf/core/error.hpp"
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

std::unique_ptr<TimerQueue> make_queue(
    const std::size_t max_timers = TimerQueueOptions::kDefaultMaxTimers) {
    auto options_result = TimerQueueOptions::create(max_timers);
    EXPECT_TRUE(options_result);
    if (!options_result) {
        return nullptr;
    }
    auto queue_result = TimerQueue::create(options_result.value());
    EXPECT_TRUE(queue_result);
    if (!queue_result) {
        return nullptr;
    }
    return std::move(queue_result).value();
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

    auto result = TimerQueue::create(options);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerQueueTest, AllocatesUniqueMonotonicIdsWithoutReuse) {
    auto queue = make_queue(3U);
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);

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
    ASSERT_NE(queue, nullptr);

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
    ASSERT_NE(queue, nullptr);

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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
    auto added = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(added);
    ASSERT_TRUE(queue->cancel(added.value()));

    auto second = queue->cancel(added.value());

    ASSERT_TRUE(second);
    EXPECT_EQ(second.value(), TimerCancelOutcome::NotFound);
}

TEST(TimerQueueTest, RejectsInvalidTimerId) {
    auto queue = make_queue(1U);
    ASSERT_NE(queue, nullptr);

    auto result = queue->cancel(TimerId{});

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(TimerQueueTest, CancelsPendingDispatchBeforeCallbackStarts) {
    auto queue = make_queue(1U);
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
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
    ASSERT_NE(queue, nullptr);
    auto added = queue->run_at(TimerQueue::TimePoint{}, [] {});
    ASSERT_TRUE(added);
    TimerQueueTestAccess::make_dispatching(*queue, added.value(), true);

    auto result = queue->cancel(added.value());

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), TimerCancelOutcome::TooLate);
    EXPECT_TRUE(
        TimerQueueTestAccess::cancel_requested(*queue, added.value()));
}

}  // namespace
