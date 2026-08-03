#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "iaisf/net/timer.hpp"

namespace iaisf::net::detail {

struct TimerIdLess final {
    [[nodiscard]] bool operator()(TimerId lhs, TimerId rhs) const noexcept {
        return lhs.sequence_ < rhs.sequence_;
    }
};

/**
 * Pure in-memory timer index used by the Linux timer implementation.
 *
 * Phase 8A-1 deliberately has no timerfd, Channel, EventLoop, locking, or
 * dispatch behavior. The eventual EventLoop owner thread is responsible for
 * serializing access.
 */
class TimerQueue final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    [[nodiscard]] static Result<std::unique_ptr<TimerQueue>> create(
        const TimerQueueOptions& options);

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;
    TimerQueue(TimerQueue&&) = delete;
    TimerQueue& operator=(TimerQueue&&) = delete;
    ~TimerQueue() = default;

    [[nodiscard]] Result<TimerId> run_at(
        TimePoint deadline,
        const TimerCallback& callback);
    [[nodiscard]] Result<TimerId> run_after(
        Duration delay,
        const TimerCallback& callback);
    [[nodiscard]] Result<TimerCancelOutcome> cancel(TimerId id);

private:
    struct DeadlineKey final {
        TimePoint deadline;
        std::uint64_t sequence;
    };

    struct DeadlineKeyLess final {
        [[nodiscard]] bool operator()(
            const DeadlineKey& lhs,
            const DeadlineKey& rhs) const noexcept {
            if (lhs.deadline != rhs.deadline) {
                return lhs.deadline < rhs.deadline;
            }
            return lhs.sequence < rhs.sequence;
        }
    };

    using ScheduleMap = std::map<DeadlineKey, TimerId, DeadlineKeyLess>;

    enum class TimerRecordState {
        Scheduled,
        PendingDispatch,
        DispatchingOneShot,
        DispatchingRepeating,
    };

    struct TimerRecord final {
        TimerCallback callback;
        std::optional<Duration> interval;
        bool cancel_requested{false};
        TimerRecordState state{TimerRecordState::Scheduled};
        std::optional<ScheduleMap::iterator> schedule_iterator;
    };

    using IdIndex = std::map<TimerId, TimerRecord, TimerIdLess>;

    enum class AddFailurePoint {
        None,
        BeforeScheduleInsert,
        BeforeIdIndexInsert,
    };

    explicit TimerQueue(TimerQueueOptions options) noexcept;

    [[nodiscard]] Result<TimerId> add(
        TimePoint deadline,
        const TimerCallback& callback,
        std::optional<Duration> interval);

    TimerQueueOptions options_;
    ScheduleMap schedule_;
    IdIndex id_index_;
    std::uint64_t next_sequence_{1U};
    bool id_exhausted_{false};
    AddFailurePoint add_failure_point_{AddFailurePoint::None};

    friend class TimerQueueTestAccess;
};

/** Test-only observer and deterministic failure seam for the internal queue. */
class TimerQueueTestAccess final {
public:
    enum class AddFailurePoint {
        BeforeScheduleInsert,
        BeforeIdIndexInsert,
    };

    [[nodiscard]] static std::size_t timer_count(const TimerQueue& queue) noexcept;
    [[nodiscard]] static std::vector<TimerId> scheduled_ids(const TimerQueue& queue);
    [[nodiscard]] static std::uint64_t sequence(TimerId id) noexcept;
    [[nodiscard]] static bool contains(const TimerQueue& queue, TimerId id) noexcept;
    [[nodiscard]] static bool cancel_requested(
        const TimerQueue& queue,
        TimerId id) noexcept;

    static void set_next_sequence(
        TimerQueue& queue,
        std::uint64_t next_sequence,
        bool exhausted = false) noexcept;
    static void fail_next_add(
        TimerQueue& queue,
        AddFailurePoint failure_point) noexcept;
    static void make_pending_dispatch(TimerQueue& queue, TimerId id);
    static void make_dispatching(
        TimerQueue& queue,
        TimerId id,
        bool repeating);
};

}  // namespace iaisf::net::detail
