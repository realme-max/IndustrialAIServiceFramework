#include "timer_queue.hpp"

#include <exception>
#include <limits>
#include <new>
#include <utility>

#include "iaisf/core/error.hpp"

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

TimerQueue::TimerQueue(TimerQueueOptions options) noexcept
    : options_(std::move(options)) {}

Result<std::unique_ptr<TimerQueue>> TimerQueue::create(
    const TimerQueueOptions& options) {
    auto validated_options = TimerQueueOptions::create(options.max_timers);
    if (!validated_options) {
        return Result<std::unique_ptr<TimerQueue>>::failure(
            validated_options.error());
    }

    try {
        return Result<std::unique_ptr<TimerQueue>>::success(
            std::unique_ptr<TimerQueue>{
                new TimerQueue{std::move(validated_options).value()}});
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

Result<TimerId> TimerQueue::add(
    const TimePoint deadline,
    const TimerCallback& callback,
    std::optional<Duration> interval) {
    if (!callback) {
        return Result<TimerId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "timer callback cannot be empty"));
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

    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        id_exhausted_ = true;
    } else {
        ++next_sequence_;
    }
    return Result<TimerId>::success(candidate);
}

Result<TimerCancelOutcome> TimerQueue::cancel(const TimerId id) {
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
        case TimerRecordState::Scheduled:
            if (!record.schedule_iterator.has_value()) {
                return Result<TimerCancelOutcome>::failure(make_error(
                    ErrorCode::InternalError,
                    "scheduled timer has no schedule index"));
            }
            schedule_.erase(*record.schedule_iterator);
            id_index_.erase(record_iterator);
            return Result<TimerCancelOutcome>::success(
                TimerCancelOutcome::Cancelled);

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
