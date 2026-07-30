#include "iaisf/task/task_repository.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace iaisf::task {

TaskRepository::TaskRepository(TaskLimits limits) : limits_(std::move(limits)) {}

Result<TaskId> TaskRepository::create_queued(const TaskRequest& request) {
    const auto valid = limits_.validate_request(request);
    if (!valid) {
        return Result<TaskId>::failure(valid.error());
    }

    Record record;
    try {
        record.request = request;
    } catch (const std::bad_alloc&) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to copy task request into repository storage"));
    } catch (const std::length_error&) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task request exceeds the platform size limit"));
    } catch (const nlohmann::json::exception&) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task request cannot be copied"));
    }
    record.created_at = std::chrono::system_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.size() >= limits_.max_repository_tasks()) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task repository capacity has been reached"));
    }
    if (id_exhausted_) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task identifier space has been exhausted"));
    }

    record.id = TaskId{next_id_};
    try {
        const auto inserted = records_.emplace(record.id, std::move(record));
        if (!inserted.second) {
            return Result<TaskId>::failure(make_error(
                ErrorCode::InternalError,
                "generated task identifier already exists"));
        }
    } catch (const std::bad_alloc&) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate task repository storage"));
    } catch (const std::length_error&) {
        return Result<TaskId>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task repository exceeds the platform size limit"));
    }

    const TaskId id{next_id_};
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        id_exhausted_ = true;
    } else {
        ++next_id_;
    }
    return Result<TaskId>::success(id);
}

Result<void> TaskRepository::rollback_queued(const TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return Result<void>::failure(
            make_error(ErrorCode::NotFound, "task was not found"));
    }
    if (found->second.state != TaskState::Queued) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "only a queued task can be rolled back"));
    }
    records_.erase(found);
    return Result<void>::success();
}

Result<TransitionOutcome> TaskRepository::mark_running(const TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return Result<TransitionOutcome>::failure(
            make_error(ErrorCode::NotFound, "task was not found"));
    }
    auto& record = found->second;
    if (is_terminal(record.state)) {
        return Result<TransitionOutcome>::success(
            TransitionOutcome::AlreadyTerminal);
    }
    if (record.state != TaskState::Queued) {
        return Result<TransitionOutcome>::failure(make_error(
            ErrorCode::InvalidState,
            "task is not queued"));
    }
    record.state = TaskState::Running;
    record.started_at = ordered_now(record.created_at);
    return Result<TransitionOutcome>::success(TransitionOutcome::Applied);
}

Result<TransitionOutcome> TaskRepository::mark_succeeded(
    const TaskId id,
    const nlohmann::json& result) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = records_.find(id);
        if (found == records_.end()) {
            return Result<TransitionOutcome>::failure(
                make_error(ErrorCode::NotFound, "task was not found"));
        }
        if (is_terminal(found->second.state)) {
            return Result<TransitionOutcome>::success(
                TransitionOutcome::AlreadyTerminal);
        }
        if (found->second.state != TaskState::Running) {
            return Result<TransitionOutcome>::failure(make_error(
                ErrorCode::InvalidState,
                "only a running task can succeed"));
        }
    }

    const auto valid = limits_.validate_result(result);
    if (!valid) {
        return Result<TransitionOutcome>::failure(valid.error());
    }

    nlohmann::json owned_result;
    try {
        owned_result = result;
    } catch (const std::bad_alloc&) {
        return Result<TransitionOutcome>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to copy task result into repository storage"));
    } catch (const std::length_error&) {
        return Result<TransitionOutcome>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task result exceeds the platform size limit"));
    } catch (const nlohmann::json::exception&) {
        return Result<TransitionOutcome>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task result cannot be copied"));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return Result<TransitionOutcome>::failure(
            make_error(ErrorCode::NotFound, "task was not found"));
    }
    auto& record = found->second;
    if (is_terminal(record.state)) {
        return Result<TransitionOutcome>::success(
            TransitionOutcome::AlreadyTerminal);
    }
    if (record.state != TaskState::Running) {
        return Result<TransitionOutcome>::failure(make_error(
            ErrorCode::InvalidState,
            "only a running task can succeed"));
    }
    record.result.emplace(std::move(owned_result));
    record.error.reset();
    record.state = TaskState::Succeeded;
    record.finished_at = ordered_now(record.started_at.value_or(record.created_at));
    return Result<TransitionOutcome>::success(TransitionOutcome::Applied);
}

Result<TransitionOutcome> TaskRepository::mark_failed(
    const TaskId id,
    const Error& error) {
    return mark_error_terminal(id, TaskState::Failed, error);
}

Result<TransitionOutcome> TaskRepository::mark_timed_out(
    const TaskId id,
    const Error& error) {
    return mark_error_terminal(id, TaskState::TimedOut, error);
}

Result<TaskSnapshot> TaskRepository::get_snapshot(const TaskId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return Result<TaskSnapshot>::failure(
            make_error(ErrorCode::NotFound, "task was not found"));
    }

    try {
        const auto& record = found->second;
        TaskSnapshot snapshot;
        snapshot.id = record.id;
        snapshot.operation = record.request.operation;
        snapshot.state = record.state;
        snapshot.created_at = record.created_at;
        snapshot.started_at = record.started_at;
        snapshot.finished_at = record.finished_at;
        snapshot.result = record.result;
        snapshot.error = record.error;
        return Result<TaskSnapshot>::success(std::move(snapshot));
    } catch (const std::bad_alloc&) {
        return Result<TaskSnapshot>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to copy task snapshot"));
    } catch (const std::length_error&) {
        return Result<TaskSnapshot>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "task snapshot exceeds the platform size limit"));
    } catch (const nlohmann::json::exception&) {
        return Result<TaskSnapshot>::failure(make_error(
            ErrorCode::InternalError,
            "stored task snapshot cannot be copied"));
    }
}

Result<void> TaskRepository::erase_terminal(const TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return Result<void>::failure(
            make_error(ErrorCode::NotFound, "task was not found"));
    }
    if (!is_terminal(found->second.state)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "only a terminal task can be erased"));
    }
    records_.erase(found);
    return Result<void>::success();
}

std::size_t TaskRepository::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

std::size_t TaskRepository::capacity() const noexcept {
    return limits_.max_repository_tasks();
}

const TaskLimits& TaskRepository::limits() const noexcept {
    return limits_;
}

Result<TransitionOutcome> TaskRepository::mark_error_terminal(
    const TaskId id,
    const TaskState terminal_state,
    const Error& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = records_.find(id);
        if (found == records_.end()) {
            return Result<TransitionOutcome>::failure(
                make_error(ErrorCode::NotFound, "task was not found"));
        }
        if (is_terminal(found->second.state)) {
            return Result<TransitionOutcome>::success(
                TransitionOutcome::AlreadyTerminal);
        }
        if (found->second.state != TaskState::Running) {
            return Result<TransitionOutcome>::failure(make_error(
                ErrorCode::InvalidState,
                "only a running task can enter an error terminal state"));
        }
    }

    auto sanitized = limits_.sanitize_error(error);
    if (!sanitized) {
        return Result<TransitionOutcome>::failure(std::move(sanitized).error());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        return Result<TransitionOutcome>::failure(
            make_error(ErrorCode::NotFound, "task was not found"));
    }
    auto& record = found->second;
    if (is_terminal(record.state)) {
        return Result<TransitionOutcome>::success(
            TransitionOutcome::AlreadyTerminal);
    }
    if (record.state != TaskState::Running) {
        return Result<TransitionOutcome>::failure(make_error(
            ErrorCode::InvalidState,
            "only a running task can enter an error terminal state"));
    }
    record.error.emplace(std::move(sanitized).value());
    record.result.reset();
    record.state = terminal_state;
    record.finished_at = ordered_now(record.started_at.value_or(record.created_at));
    return Result<TransitionOutcome>::success(TransitionOutcome::Applied);
}

std::chrono::system_clock::time_point TaskRepository::ordered_now(
    const std::chrono::system_clock::time_point lower_bound) noexcept {
    return std::max(std::chrono::system_clock::now(), lower_bound);
}

}  // namespace iaisf::task
