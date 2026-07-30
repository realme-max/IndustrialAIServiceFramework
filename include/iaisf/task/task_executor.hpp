#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

#include "iaisf/logging/logger.hpp"
#include "iaisf/task/task_repository.hpp"
#include "iaisf/task/task_types.hpp"

namespace iaisf::task {

/**
 * Worker-side boundary that isolates handler and logger failures.
 *
 * The injected handler may execute concurrently and must therefore be
 * thread-safe. execute() never lets a user exception escape a worker.
 */
class TaskExecutor {
public:
    TaskExecutor(TaskRepository& repository, ILogger& logger, TaskHandler handler);

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    void execute(TaskId id, const TaskRequest& request) noexcept;

    [[nodiscard]] std::size_t late_completion_count() const noexcept;
    [[nodiscard]] std::size_t handler_exception_count() const noexcept;
    [[nodiscard]] std::size_t logger_failure_count() const noexcept;

private:
    void complete_with_error(TaskId id, Error error) noexcept;
    void observe_transition(const Result<TransitionOutcome>& transition) noexcept;
    void safe_log(std::string_view message) noexcept;

    TaskRepository& repository_;
    ILogger& logger_;
    TaskHandler handler_;
    std::atomic<std::size_t> late_completion_count_{0};
    std::atomic<std::size_t> handler_exception_count_{0};
    std::atomic<std::size_t> logger_failure_count_{0};
};

}  // namespace iaisf::task
