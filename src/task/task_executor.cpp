#include "iaisf/task/task_executor.hpp"

#include <exception>
#include <utility>

namespace iaisf::task {

TaskExecutor::TaskExecutor(
    TaskRepository& repository,
    ILogger& logger,
    TaskHandler handler)
    : repository_(repository), logger_(logger), handler_(std::move(handler)) {}

void TaskExecutor::execute(const TaskId id, const TaskRequest& request) noexcept {
    try {
        const auto running = repository_.mark_running(id);
        if (!running) {
            safe_log("task could not enter running state");
            return;
        }
        if (running.value() == TransitionOutcome::AlreadyTerminal) {
            late_completion_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        try {
            auto handled = handler_(request);
            if (!handled) {
                complete_with_error(id, std::move(handled).error());
                return;
            }

            auto completed = repository_.mark_succeeded(id, handled.value());
            if (!completed &&
                (completed.error().code == ErrorCode::ResourceExhausted ||
                 completed.error().code == ErrorCode::InvalidArgument)) {
                complete_with_error(
                    id,
                    make_error(
                        ErrorCode::InternalError,
                        "task handler produced an invalid or oversized result"));
                return;
            }
            observe_transition(completed);
        } catch (...) {
            handler_exception_count_.fetch_add(1, std::memory_order_relaxed);
            safe_log("task handler threw an exception");
            complete_with_error(
                id,
                make_error(ErrorCode::InternalError, "task handler failed"));
        }
    } catch (...) {
        safe_log("task executor isolated an internal exception");
    }
}

std::size_t TaskExecutor::late_completion_count() const noexcept {
    return late_completion_count_.load(std::memory_order_relaxed);
}

std::size_t TaskExecutor::handler_exception_count() const noexcept {
    return handler_exception_count_.load(std::memory_order_relaxed);
}

std::size_t TaskExecutor::logger_failure_count() const noexcept {
    return logger_failure_count_.load(std::memory_order_relaxed);
}

void TaskExecutor::complete_with_error(const TaskId id, Error error) noexcept {
    try {
        const auto completed = repository_.mark_failed(id, std::move(error));
        observe_transition(completed);
    } catch (...) {
        safe_log("task failure transition raised an internal exception");
    }
}

void TaskExecutor::observe_transition(
    const Result<TransitionOutcome>& transition) noexcept {
    if (!transition) {
        if (transition.error().code == ErrorCode::NotFound) {
            late_completion_count_.fetch_add(1, std::memory_order_relaxed);
            safe_log("late task completion found no retained task record");
            return;
        }
        safe_log("task terminal transition failed");
        return;
    }
    if (transition.value() == TransitionOutcome::AlreadyTerminal) {
        late_completion_count_.fetch_add(1, std::memory_order_relaxed);
        safe_log("late task completion was discarded");
    }
}

void TaskExecutor::safe_log(const std::string_view message) noexcept {
    try {
        logger_.log(LogLevel::Error, "task_executor", message);
    } catch (...) {
        logger_failure_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace iaisf::task
