#include "timer_queue.hpp"

#include <cerrno>
#include <chrono>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include <sys/timerfd.h>
#include <unistd.h>

#include "iaisf/core/error.hpp"
#include "iaisf/net/channel.hpp"
#include "iaisf/net/event_loop.hpp"
#include "system_error.hpp"

namespace iaisf::net {

Result<TimerQueueOptions> TimerQueueOptions::create(
    const std::size_t max_timers) {
    if (max_timers == 0U || max_timers > kHardMaxTimers) {
        return Result<TimerQueueOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer capacity must be between 1 and 1000000"));
    }
    return Result<TimerQueueOptions>::success(TimerQueueOptions{max_timers});
}

namespace detail {

TimerQueue::TimerQueue(
    EventLoop& owner,
    TimerQueueOptions options,
    UniqueFd timer_fd,
    EventNotification notification) noexcept
    : owner_(owner),
      options_(std::move(options)),
      timer_fd_(std::move(timer_fd)),
      notification_(std::move(notification)) {}

TimerQueue::~TimerQueue() noexcept {
    try {
        static_cast<void>(shutdown());
    } catch (...) {
        std::terminate();
    }
    if (timer_channel_ && timer_channel_->is_registered()) {
        std::terminate();
    }
}

Result<std::unique_ptr<TimerQueue>> TimerQueue::create(
    EventLoop& owner,
    const TimerQueueOptions& options,
    EventNotification notification) {
    auto validated_options = TimerQueueOptions::create(options.max_timers);
    if (!validated_options) {
        return Result<std::unique_ptr<TimerQueue>>::failure(
            validated_options.error());
    }
    if (!notification) {
        return Result<std::unique_ptr<TimerQueue>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer event notification cannot be empty"));
    }

    const int descriptor =
        ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (descriptor < 0) {
        const int error_number = errno;
        return Result<std::unique_ptr<TimerQueue>>::failure(
            make_system_error("timerfd_create", error_number));
    }

    UniqueFd timer_fd{descriptor};
    try {
        auto queue = std::unique_ptr<TimerQueue>{new TimerQueue{
            owner,
            std::move(validated_options).value(),
            std::move(timer_fd),
            std::move(notification)}};
        queue->expired_batch_.reserve(queue->options_.max_timers);
        auto channel_result = queue->initialize_channel();
        if (!channel_result) {
            return Result<std::unique_ptr<TimerQueue>>::failure(
                std::move(channel_result).error());
        }
        return Result<std::unique_ptr<TimerQueue>>::success(std::move(queue));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<TimerQueue>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate timer queue"));
    } catch (const std::exception&) {
        return Result<std::unique_ptr<TimerQueue>>::failure(make_error(
            ErrorCode::InternalError,
            "unable to create timer queue"));
    }
}

Result<void> TimerQueue::initialize_channel() {
    try {
        timer_channel_ = std::make_unique<Channel>(owner_, timer_fd_.get());
        timer_channel_->set_read_callback([this] { handle_readable(); });
        timer_channel_->set_error_callback(
            [this] { handle_descriptor_failure(); });
        timer_channel_->set_close_callback(
            [this] { handle_descriptor_failure(); });
        timer_channel_->enable_reading();
        timer_channel_->set_edge_triggered(true);
        return timer_channel_->update();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate timer channel"));
    } catch (const std::exception&) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "unable to initialize timer channel"));
    }
}

Result<TimerId> TimerQueue::run_at(
    const TimePoint deadline,
    const TimerCallback& callback) {
    return add(deadline, callback, std::nullopt);
}

Result<TimerId> TimerQueue::run_after(
    const Duration delay,
    const TimerCallback& callback) {
    if (delay < Duration::zero()) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer delay cannot be negative"));
    }

    const TimePoint now = Clock::now();
    if (delay > TimePoint::max() - now) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer deadline exceeds steady clock range"));
    }
    return add(now + delay, callback, std::nullopt);
}

Result<TimerId> TimerQueue::run_every(
    const Duration interval,
    const TimerCallback& callback) {
    if (interval <= Duration::zero()) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "repeating timer interval must be positive"));
    }

    const TimePoint now = Clock::now();
    if (interval > TimePoint::max() - now) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "repeating timer deadline exceeds steady clock range"));
    }
    return add(now + interval, callback, interval);
}

Result<TimerId> TimerQueue::add(
    const TimePoint deadline,
    const TimerCallback& callback,
    std::optional<Duration> interval) {
    if (shutdown_ || faulted_) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidState,
            "timer queue is not accepting timers"));
    }
    if (deadline.time_since_epoch() < Duration::zero()) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer deadline cannot precede the monotonic epoch"));
    }
    if (!callback) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer callback cannot be empty"));
    }
    if (interval.has_value() && *interval <= Duration::zero()) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "repeating timer interval must be positive"));
    }
    if (id_index_.size() >= options_.max_timers) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "timer queue capacity is exhausted"));
    }
    if (id_exhausted_) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "timer id space is exhausted"));
    }

    const TimerId candidate{next_sequence_};
    ScheduleMap::iterator schedule_iterator = schedule_.end();
    bool schedule_inserted = false;

    try {
        TimerRecord record{
            callback,
            std::move(interval),
            deadline,
            false,
            TimerRecordState::Scheduled,
            std::nullopt};

        if (add_failure_point_ == AddFailurePoint::BeforeScheduleInsert) {
            add_failure_point_ = AddFailurePoint::None;
            throw std::bad_alloc{};
        }

        const auto schedule_result = schedule_.emplace(
            DeadlineKey{deadline, next_sequence_},
            candidate);
        if (!schedule_result.second) {
            return Result<TimerId>::failure(make_error(
                ErrorCode::InternalError,
                "timer schedule key collision"));
        }
        schedule_iterator = schedule_result.first;
        schedule_inserted = true;
        record.schedule_iterator = schedule_iterator;

        if (add_failure_point_ == AddFailurePoint::BeforeIdIndexInsert) {
            add_failure_point_ = AddFailurePoint::None;
            throw std::bad_alloc{};
        }

        const auto id_result = id_index_.emplace(candidate, std::move(record));
        if (!id_result.second) {
            schedule_.erase(schedule_iterator);
            return Result<TimerId>::failure(make_error(
                ErrorCode::InternalError,
                "timer id collision"));
        }
    } catch (const std::bad_alloc&) {
        if (schedule_inserted) {
            schedule_.erase(schedule_iterator);
        }
        return Result<TimerId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to store timer"));
    } catch (const std::exception&) {
        if (schedule_inserted) {
            schedule_.erase(schedule_iterator);
        }
        return Result<TimerId>::failure(make_error(
            ErrorCode::InternalError,
            "unable to store timer"));
    } catch (...) {
        if (schedule_inserted) {
            schedule_.erase(schedule_iterator);
        }
        return Result<TimerId>::failure(make_error(
            ErrorCode::InternalError,
            "unable to store timer"));
    }

    if (schedule_iterator == schedule_.begin()) {
        auto arm_result = arm_next_timer();
        if (!arm_result) {
            id_index_.erase(candidate);
            schedule_.erase(schedule_iterator);
            fail_closed("failed to arm timerfd after scheduling a timer");
            return Result<TimerId>::failure(std::move(arm_result).error());
        }
    }

    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        id_exhausted_ = true;
    } else {
        ++next_sequence_;
    }
    return Result<TimerId>::success(candidate);
}

Result<TimerCancelOutcome> TimerQueue::cancel(const TimerId id) {
    if (shutdown_ || faulted_) {
        return Result<TimerCancelOutcome>::failure(make_error(
            ErrorCode::InvalidState,
            "timer queue cannot cancel timers in its current state"));
    }
    if (!id.valid()) {
        return Result<TimerCancelOutcome>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer id is invalid"));
    }

    const auto record_iterator = id_index_.find(id);
    if (record_iterator == id_index_.end()) {
        return Result<TimerCancelOutcome>::success(
            TimerCancelOutcome::NotFound);
    }

    auto& record = record_iterator->second;
    switch (record.state) {
        case TimerRecordState::Scheduled: {
            if (!record.schedule_iterator.has_value()) {
                return Result<TimerCancelOutcome>::failure(make_error(
                    ErrorCode::InternalError,
                    "scheduled timer has no schedule index"));
            }
            const bool was_earliest =
                *record.schedule_iterator == schedule_.begin();
            auto schedule_node = schedule_.extract(*record.schedule_iterator);
            auto id_node = id_index_.extract(record_iterator);

            if (was_earliest) {
                auto arm_result = arm_next_timer();
                if (!arm_result) {
                    const auto restored_schedule =
                        schedule_.insert(std::move(schedule_node));
                    id_node.mapped().schedule_iterator =
                        restored_schedule.position;
                    id_index_.insert(std::move(id_node));
                    fail_closed("failed to rearm timerfd after cancellation");
                    return Result<TimerCancelOutcome>::failure(
                        std::move(arm_result).error());
                }
            }
            return Result<TimerCancelOutcome>::success(
                TimerCancelOutcome::Cancelled);
        }

        case TimerRecordState::PendingDispatch:
            id_index_.erase(record_iterator);
            return Result<TimerCancelOutcome>::success(
                TimerCancelOutcome::Cancelled);

        case TimerRecordState::DispatchingOneShot:
            return Result<TimerCancelOutcome>::success(
                TimerCancelOutcome::TooLate);

        case TimerRecordState::DispatchingRepeating:
            record.cancel_requested = true;
            return Result<TimerCancelOutcome>::success(
                TimerCancelOutcome::TooLate);
    }

    return Result<TimerCancelOutcome>::failure(make_error(
        ErrorCode::InternalError,
        "timer has an unknown state"));
}

Result<void> TimerQueue::arm_next_timer() {
    if (schedule_.empty()) {
        return disarm_timer();
    }
    return set_timer(schedule_.begin()->first.deadline);
}

Result<void> TimerQueue::set_timer(const TimePoint deadline) {
    if (fail_next_arm_) {
        fail_next_arm_ = false;
        return Result<void>::failure(make_error(
            ErrorCode::SystemError,
            "injected timerfd_settime failure"));
    }

    auto since_epoch = deadline.time_since_epoch();
    if (since_epoch < Duration::zero()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer deadline cannot precede the monotonic epoch"));
    }
    if (since_epoch == Duration::zero()) {
        since_epoch = std::chrono::nanoseconds{1};
    }

    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        since_epoch);
    if (seconds.count() >
        static_cast<decltype(seconds.count())>(
            std::numeric_limits<time_t>::max())) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer deadline exceeds timerfd range"));
    }
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            since_epoch - seconds);

    itimerspec setting{};
    setting.it_value.tv_sec = static_cast<time_t>(seconds.count());
    setting.it_value.tv_nsec = static_cast<long>(nanoseconds.count());
    if (::timerfd_settime(
            timer_fd_.get(),
            TFD_TIMER_ABSTIME,
            &setting,
            nullptr) != 0) {
        const int error_number = errno;
        return Result<void>::failure(
            make_system_error("timerfd_settime", error_number));
    }
    return Result<void>::success();
}

Result<void> TimerQueue::disarm_timer() {
    if (fail_next_arm_) {
        fail_next_arm_ = false;
        return Result<void>::failure(make_error(
            ErrorCode::SystemError,
            "injected timerfd_settime failure"));
    }

    const itimerspec setting{};
    if (::timerfd_settime(timer_fd_.get(), 0, &setting, nullptr) != 0) {
        const int error_number = errno;
        return Result<void>::failure(
            make_system_error("timerfd_settime", error_number));
    }
    return Result<void>::success();
}

Result<void> TimerQueue::drain_timer_fd() {
    if (fail_next_read_) {
        fail_next_read_ = false;
        return Result<void>::failure(make_error(
            ErrorCode::SystemError,
            "injected timerfd read failure"));
    }

    std::uint64_t expiration_count = 0U;
    while (true) {
        const ssize_t bytes_read =
            ::read(timer_fd_.get(), &expiration_count, sizeof(expiration_count));
        if (bytes_read == static_cast<ssize_t>(sizeof(expiration_count))) {
            continue;
        }
        if (bytes_read < 0) {
            const int error_number = errno;
            if (error_number == EINTR) {
                continue;
            }
            if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
                return Result<void>::success();
            }
            return Result<void>::failure(
                make_system_error("timerfd read", error_number));
        }
        return Result<void>::failure(make_error(
            ErrorCode::SystemError,
            "timerfd read produced an incomplete result"));
    }
}

Result<void> TimerQueue::collect_expired(const TimePoint now) {
    expired_batch_.clear();
    while (!schedule_.empty() && schedule_.begin()->first.deadline <= now) {
        const auto schedule_iterator = schedule_.begin();
        const TimerId id = schedule_iterator->second;
        const auto record_iterator = id_index_.find(id);
        if (record_iterator == id_index_.end()) {
            return Result<void>::failure(make_error(
                ErrorCode::InternalError,
                "timer schedule has no id index record"));
        }
        expired_batch_.push_back(id);
        record_iterator->second.state = TimerRecordState::PendingDispatch;
        record_iterator->second.schedule_iterator.reset();
        schedule_.erase(schedule_iterator);
    }
    return arm_next_timer();
}

Result<TimerQueue::TimePoint> TimerQueue::next_repeating_deadline(
    const TimePoint previous_deadline,
    const Duration interval,
    const TimePoint now) {
    if (interval <= Duration::zero()) {
        return Result<TimePoint>::failure(make_error(
            ErrorCode::InvalidArgument,
            "repeating timer interval must be positive"));
    }
    if (previous_deadline > TimePoint::max() - interval) {
        return Result<TimePoint>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "repeating timer deadline exceeds steady clock range"));
    }

    const TimePoint nominal_next = previous_deadline + interval;
    if (nominal_next > now) {
        return Result<TimePoint>::success(nominal_next);
    }

    // Preserve the fixed-rate phase while skipping every missed interval.
    // This arithmetic is constant-time and never replays missed callbacks.
    const Duration overdue = now - nominal_next;
    const Duration remainder = overdue % interval;
    const Duration advance = interval - remainder;
    if (advance > TimePoint::max() - now) {
        return Result<TimePoint>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "repeating timer deadline exceeds steady clock range"));
    }
    return Result<TimePoint>::success(now + advance);
}

void TimerQueue::handle_readable() noexcept {
    if (faulted_ || shutdown_) {
        return;
    }
    try {
        auto drain_result = drain_timer_fd();
        if (!drain_result) {
            fail_closed("failed to drain timerfd");
            return;
        }
        auto collect_result = collect_expired(Clock::now());
        if (!collect_result) {
            fail_closed("failed to collect or rearm expired timers");
            return;
        }
        dispatch_expired();
    } catch (const std::exception&) {
        fail_closed("timerfd dispatch threw std::exception");
    } catch (...) {
        fail_closed("timerfd dispatch threw an unknown exception");
    }
}

void TimerQueue::handle_descriptor_failure() noexcept {
    fail_closed("timerfd reported an error or hangup");
}

void TimerQueue::dispatch_expired() noexcept {
    for (std::size_t index = 0U; index < expired_batch_.size(); ++index) {
        const TimerId id = expired_batch_[index];
        if (owner_.state() != EventLoop::State::Running || faulted_) {
            for (; index < expired_batch_.size(); ++index) {
                const auto pending = id_index_.find(expired_batch_[index]);
                if (pending != id_index_.end() &&
                    pending->second.state == TimerRecordState::PendingDispatch) {
                    id_index_.erase(pending);
                }
            }
            return;
        }

        TimerCallback callback;
        std::optional<Duration> interval;
        TimePoint previous_deadline{};
        {
            const auto record_iterator = id_index_.find(id);
            if (record_iterator == id_index_.end()) {
                continue;
            }
            if (record_iterator->second.state !=
                TimerRecordState::PendingDispatch) {
                fail_closed("timer pending-dispatch state is inconsistent");
                return;
            }
            auto& record = record_iterator->second;
            interval = record.interval;
            previous_deadline = record.deadline;
            record.state = interval.has_value()
                               ? TimerRecordState::DispatchingRepeating
                               : TimerRecordState::DispatchingOneShot;
            callback = std::move(record.callback);
        }

        bool callback_failed = false;
        try {
            callback();
        } catch (const std::exception&) {
            callback_failed = true;
            notify("timer callback threw std::exception", false);
        } catch (...) {
            callback_failed = true;
            notify("timer callback threw an unknown exception", false);
        }

        auto record_iterator = id_index_.find(id);
        if (record_iterator == id_index_.end()) {
            continue;
        }
        auto& record = record_iterator->second;
        if (!interval.has_value() || callback_failed ||
            record.cancel_requested || shutdown_ || faulted_ ||
            owner_.state() != EventLoop::State::Running) {
            id_index_.erase(record_iterator);
            continue;
        }

        auto deadline_result = next_repeating_deadline(
            previous_deadline,
            *interval,
            Clock::now());
        if (!deadline_result) {
            id_index_.erase(record_iterator);
            fail_closed("repeating timer deadline overflowed");
            return;
        }
        const TimePoint next_deadline = std::move(deadline_result).value();

        ScheduleMap::iterator schedule_iterator = schedule_.end();
        try {
            const auto schedule_result = schedule_.emplace(
                DeadlineKey{next_deadline, id.sequence_},
                id);
            if (!schedule_result.second) {
                id_index_.erase(record_iterator);
                fail_closed("repeating timer schedule key collided");
                return;
            }
            schedule_iterator = schedule_result.first;
        } catch (const std::exception&) {
            id_index_.erase(record_iterator);
            fail_closed("unable to reschedule repeating timer");
            return;
        } catch (...) {
            id_index_.erase(record_iterator);
            fail_closed("unable to reschedule repeating timer");
            return;
        }

        record.callback = std::move(callback);
        record.deadline = next_deadline;
        record.state = TimerRecordState::Scheduled;
        record.schedule_iterator = schedule_iterator;

        if (schedule_iterator == schedule_.begin()) {
            auto arm_result = arm_next_timer();
            if (!arm_result) {
                schedule_.erase(schedule_iterator);
                id_index_.erase(record_iterator);
                fail_closed("failed to rearm repeating timer");
                return;
            }
        }
    }
}

void TimerQueue::fail_closed(const char* const message) noexcept {
    if (faulted_ || shutdown_) {
        return;
    }
    faulted_ = true;
    notify(message, true);
}

void TimerQueue::notify(
    const char* const message,
    const bool request_stop) noexcept {
    try {
        notification_(message, request_stop);
    } catch (...) {
        notification_failed_ = true;
    }
}

Result<void> TimerQueue::shutdown() {
    shutdown_ = true;
    faulted_ = true;
    schedule_.clear();
    id_index_.clear();
    expired_batch_.clear();

    std::optional<Error> first_error;
    if (timer_fd_.valid()) {
        auto disarm_result = disarm_timer();
        if (!disarm_result) {
            first_error = std::move(disarm_result).error();
        }
    }
    if (timer_channel_ && timer_channel_->is_registered()) {
        auto remove_result = timer_channel_->remove();
        if (!remove_result && !first_error.has_value()) {
            first_error = std::move(remove_result).error();
        }
    }
    if (timer_channel_ && !timer_channel_->is_registered()) {
        timer_channel_.reset();
    }

    if (first_error.has_value()) {
        return Result<void>::failure(std::move(*first_error));
    }
    return Result<void>::success();
}

std::size_t TimerQueueTestAccess::timer_count(const TimerQueue& queue) noexcept {
    return queue.id_index_.size();
}

std::vector<TimerId> TimerQueueTestAccess::scheduled_ids(
    const TimerQueue& queue) {
    std::vector<TimerId> ids;
    ids.reserve(queue.schedule_.size());
    for (const auto& entry : queue.schedule_) {
        ids.push_back(entry.second);
    }
    return ids;
}

std::uint64_t TimerQueueTestAccess::sequence(const TimerId id) noexcept {
    return id.sequence_;
}

bool TimerQueueTestAccess::contains(
    const TimerQueue& queue,
    const TimerId id) noexcept {
    return queue.id_index_.find(id) != queue.id_index_.end();
}

bool TimerQueueTestAccess::cancel_requested(
    const TimerQueue& queue,
    const TimerId id) noexcept {
    const auto iterator = queue.id_index_.find(id);
    return iterator != queue.id_index_.end() &&
           iterator->second.cancel_requested;
}

TimerQueue& TimerQueueTestAccess::queue(EventLoop& loop) noexcept {
    return *loop.timer_queue_;
}

int TimerQueueTestAccess::timer_fd(const TimerQueue& queue) noexcept {
    return queue.timer_fd_.get();
}

bool TimerQueueTestAccess::faulted(const TimerQueue& queue) noexcept {
    return queue.faulted_;
}

Result<TimerQueue::TimePoint> TimerQueueTestAccess::next_repeating_deadline(
    const TimerQueue::TimePoint previous_deadline,
    const TimerQueue::Duration interval,
    const TimerQueue::TimePoint now) {
    return TimerQueue::next_repeating_deadline(
        previous_deadline,
        interval,
        now);
}

void TimerQueueTestAccess::set_next_sequence(
    TimerQueue& queue,
    const std::uint64_t next_sequence,
    const bool exhausted) noexcept {
    queue.next_sequence_ = next_sequence;
    queue.id_exhausted_ = exhausted;
}

void TimerQueueTestAccess::fail_next_add(
    TimerQueue& queue,
    const AddFailurePoint failure_point) noexcept {
    switch (failure_point) {
        case AddFailurePoint::BeforeScheduleInsert:
            queue.add_failure_point_ =
                TimerQueue::AddFailurePoint::BeforeScheduleInsert;
            break;
        case AddFailurePoint::BeforeIdIndexInsert:
            queue.add_failure_point_ =
                TimerQueue::AddFailurePoint::BeforeIdIndexInsert;
            break;
    }
}

void TimerQueueTestAccess::fail_next_arm(TimerQueue& queue) noexcept {
    queue.fail_next_arm_ = true;
}

void TimerQueueTestAccess::fail_next_read(TimerQueue& queue) noexcept {
    queue.fail_next_read_ = true;
}

void TimerQueueTestAccess::set_record_deadline(
    TimerQueue& queue,
    const TimerId id,
    const TimerQueue::TimePoint deadline) {
    const auto iterator = queue.id_index_.find(id);
    if (iterator == queue.id_index_.end()) {
        throw std::logic_error{"timer id is not present"};
    }
    iterator->second.deadline = deadline;
}

void TimerQueueTestAccess::make_pending_dispatch(
    TimerQueue& queue,
    const TimerId id) {
    const auto iterator = queue.id_index_.find(id);
    if (iterator == queue.id_index_.end()) {
        return;
    }
    auto& record = iterator->second;
    if (record.schedule_iterator.has_value()) {
        queue.schedule_.erase(*record.schedule_iterator);
        record.schedule_iterator.reset();
    }
    record.state = TimerQueue::TimerRecordState::PendingDispatch;
}

void TimerQueueTestAccess::make_dispatching(
    TimerQueue& queue,
    const TimerId id,
    const bool repeating) {
    const auto iterator = queue.id_index_.find(id);
    if (iterator == queue.id_index_.end()) {
        return;
    }
    auto& record = iterator->second;
    if (record.schedule_iterator.has_value()) {
        queue.schedule_.erase(*record.schedule_iterator);
        record.schedule_iterator.reset();
    }
    record.state = repeating
                       ? TimerQueue::TimerRecordState::DispatchingRepeating
                       : TimerQueue::TimerRecordState::DispatchingOneShot;
}

}  // namespace detail
}  // namespace iaisf::net
