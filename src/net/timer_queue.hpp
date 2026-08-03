#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "iaisf/net/timer.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net {

class EventLoop;
class Channel;

namespace detail {

struct TimerIdLess final {
    [[nodiscard]] bool operator()(TimerId lhs, TimerId rhs) const noexcept {
        return lhs.sequence_ < rhs.sequence_;
    }
};

/**
 * Owner-thread-only timerfd scheduler embedded in one EventLoop.
 *
 * The queue owns its timerfd and Channel but only borrows the EventLoop. It
 * never adds locking or cross-thread scheduling; EventLoop serializes all
 * access and performs final Channel removal outside an active event batch.
 */
class TimerQueue final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    using EventNotification = std::function<void(const char*, bool)>;

    [[nodiscard]] static Result<std::unique_ptr<TimerQueue>> create(
        EventLoop& owner,
        const TimerQueueOptions& options,
        EventNotification notification);

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;
    TimerQueue(TimerQueue&&) = delete;
    TimerQueue& operator=(TimerQueue&&) = delete;
    ~TimerQueue() noexcept;

    [[nodiscard]] Result<TimerId> run_at(
        TimePoint deadline,
        const TimerCallback& callback);
    [[nodiscard]] Result<TimerId> run_after(
        Duration delay,
        const TimerCallback& callback);
    [[nodiscard]] Result<TimerCancelOutcome> cancel(TimerId id);
    [[nodiscard]] Result<void> shutdown();

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

    TimerQueue(
        EventLoop& owner,
        TimerQueueOptions options,
        UniqueFd timer_fd,
        EventNotification notification) noexcept;

    [[nodiscard]] Result<void> initialize_channel();
    [[nodiscard]] Result<TimerId> add(
        TimePoint deadline,
        const TimerCallback& callback,
        std::optional<Duration> interval);
    [[nodiscard]] Result<void> arm_next_timer();
    [[nodiscard]] Result<void> set_timer(TimePoint deadline);
    [[nodiscard]] Result<void> disarm_timer();
    [[nodiscard]] Result<void> drain_timer_fd();
    [[nodiscard]] Result<void> collect_expired(TimePoint now);
    void handle_readable() noexcept;
    void handle_descriptor_failure() noexcept;
    void dispatch_expired() noexcept;
    void fail_closed(const char* message) noexcept;
    void notify(const char* message, bool request_stop) noexcept;

    EventLoop& owner_;
    TimerQueueOptions options_;
    UniqueFd timer_fd_;
    std::unique_ptr<Channel> timer_channel_;
    EventNotification notification_;
    ScheduleMap schedule_;
    IdIndex id_index_;
    std::vector<TimerId> expired_batch_;
    std::uint64_t next_sequence_{1U};
    bool id_exhausted_{false};
    bool faulted_{false};
    bool shutdown_{false};
    bool fail_next_arm_{false};
    bool fail_next_read_{false};
    bool notification_failed_{false};
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
    [[nodiscard]] static TimerQueue& queue(EventLoop& loop) noexcept;
    [[nodiscard]] static int timer_fd(const TimerQueue& queue) noexcept;
    [[nodiscard]] static bool faulted(const TimerQueue& queue) noexcept;

    static void set_next_sequence(
        TimerQueue& queue,
        std::uint64_t next_sequence,
        bool exhausted = false) noexcept;
    static void fail_next_add(
        TimerQueue& queue,
        AddFailurePoint failure_point) noexcept;
    static void fail_next_arm(TimerQueue& queue) noexcept;
    static void fail_next_read(TimerQueue& queue) noexcept;
    static void make_pending_dispatch(TimerQueue& queue, TimerId id);
    static void make_dispatching(
        TimerQueue& queue,
        TimerId id,
        bool repeating);
};

}  // namespace detail
}  // namespace iaisf::net
