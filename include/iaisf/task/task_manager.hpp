#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/task/task_executor.hpp"
#include "iaisf/task/task_limits.hpp"
#include "iaisf/task/task_repository.hpp"
#include "iaisf/task/task_types.hpp"
#include "iaisf/task/thread_pool.hpp"

namespace iaisf::task {

class TaskManagerTestAccess;

enum class TaskSubmitFailure {
    None,
    InvalidRequest,
    ValidationRejected,
    RepositoryCapacity,
    QueueCapacity,
    NotAccepting,
    ResourceFailure,
    InternalFailure,
};

struct TaskSubmitOutcome {
    Result<TaskId> result;
    TaskSubmitFailure failure{TaskSubmitFailure::None};
};

/**
 * Transactional facade for admission, execution, query, timeout, and cleanup.
 *
 * submit() either leaves one accepted task owned by the runtime or rolls the
 * queued repository record back. shutdown() closes admission, waits for all
 * admitted submissions, then drains accepted work and joins.
 *
 * The logger and externally referenced validator/handler dependencies must
 * outlive this manager. Validators and handlers may run concurrently, must be
 * thread-safe, and must not destroy this manager or request shutdown from a
 * worker.
 */
class TaskManager {
    struct ConstructionKey {
        explicit ConstructionKey() = default;
    };

    class SubmissionGuard {
    public:
        explicit SubmissionGuard(TaskManager& manager) noexcept;
        ~SubmissionGuard();

        SubmissionGuard(const SubmissionGuard&) = delete;
        SubmissionGuard& operator=(const SubmissionGuard&) = delete;

    private:
        TaskManager& manager_;
    };

public:
    [[nodiscard]] static Result<std::unique_ptr<TaskManager>> create(
        ThreadPoolOptions pool_options,
        TaskLimits limits,
        ILogger& logger,
        TaskHandler handler);
    [[nodiscard]] static Result<std::unique_ptr<TaskManager>> create(
        ThreadPoolOptions pool_options,
        TaskLimits limits,
        ILogger& logger,
        TaskValidator validator,
        TaskHandler handler);

    TaskManager(
        ConstructionKey,
        std::unique_ptr<BoundedThreadPool> pool,
        TaskLimits limits,
        ILogger& logger,
        TaskValidator validator,
        TaskHandler handler);
    ~TaskManager();

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;
    TaskManager(TaskManager&&) = delete;
    TaskManager& operator=(TaskManager&&) = delete;

    [[nodiscard]] Result<TaskId> submit(const TaskRequest& request);
    /**
     * Same transaction as submit(), with a stable task-domain rejection
     * reason for adapters that must not classify by Error.message.
     */
    [[nodiscard]] TaskSubmitOutcome submit_with_outcome(
        const TaskRequest& request);
    [[nodiscard]] Result<TaskSnapshot> get_snapshot(TaskId id) const;
    [[nodiscard]] Result<TransitionOutcome> mark_timed_out(
        TaskId id,
        const Error& error = Error{ErrorCode::InvalidState, "task timed out"});
    [[nodiscard]] Result<void> erase_terminal(TaskId id);
    [[nodiscard]] Result<void> shutdown();

    [[nodiscard]] bool accepting() const;
    [[nodiscard]] bool stopped() const;
    [[nodiscard]] std::size_t repository_size() const;
    [[nodiscard]] std::size_t pending_count() const;
    [[nodiscard]] std::size_t task_exception_count() const noexcept;
    [[nodiscard]] std::size_t late_completion_count() const noexcept;
    [[nodiscard]] std::size_t handler_exception_count() const noexcept;
    [[nodiscard]] std::size_t logger_failure_count() const noexcept;

private:
    friend class TaskManagerTestAccess;

    [[nodiscard]] Result<void> begin_submission();
    void finish_submission() noexcept;
    [[nodiscard]] TaskSubmitOutcome submit_admitted(
        const TaskRequest& request);

    mutable std::mutex admission_mutex_;
    std::condition_variable submissions_finished_;
    bool accepting_{true};
    std::size_t in_flight_submissions_{0};

    TaskValidator validator_;
    TaskRepository repository_;
    TaskExecutor executor_;
    std::unique_ptr<BoundedThreadPool> pool_;
};

}  // namespace iaisf::task
