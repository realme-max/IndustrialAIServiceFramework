#include "iaisf/task/task_manager.hpp"

#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

namespace iaisf::task {
namespace {

TaskSubmitOutcome rejected(
    const TaskSubmitFailure failure,
    Error error) {
    return TaskSubmitOutcome{
        Result<TaskId>::failure(std::move(error)),
        failure};
}

TaskSubmitOutcome accepted(const TaskId id) {
    return TaskSubmitOutcome{
        Result<TaskId>::success(id),
        TaskSubmitFailure::None};
}

}  // namespace

Result<std::unique_ptr<TaskManager>> TaskManager::create(
    const ThreadPoolOptions pool_options,
    TaskLimits limits,
    ILogger& logger,
    TaskHandler handler) {
    return create(
        pool_options,
        std::move(limits),
        logger,
        TaskValidator{},
        std::move(handler));
}

Result<std::unique_ptr<TaskManager>> TaskManager::create(
    const ThreadPoolOptions pool_options,
    TaskLimits limits,
    ILogger& logger,
    TaskValidator validator,
    TaskHandler handler) {
    if (!handler) {
        return Result<std::unique_ptr<TaskManager>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task handler must not be empty"));
    }

    auto pool = BoundedThreadPool::create(pool_options);
    if (!pool) {
        return Result<std::unique_ptr<TaskManager>>::failure(
            std::move(pool).error());
    }

    try {
        auto manager = std::make_unique<TaskManager>(
            ConstructionKey{},
            std::move(pool).value(),
            std::move(limits),
            logger,
            std::move(validator),
            std::move(handler));
        return Result<std::unique_ptr<TaskManager>>::success(std::move(manager));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<TaskManager>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate task manager"));
    } catch (const std::exception&) {
        return Result<std::unique_ptr<TaskManager>>::failure(make_error(
            ErrorCode::InternalError,
            "unable to construct task manager"));
    }
}

TaskManager::TaskManager(
    ConstructionKey,
    std::unique_ptr<BoundedThreadPool> pool,
    TaskLimits limits,
    ILogger& logger,
    TaskValidator validator,
    TaskHandler handler)
    : validator_(std::move(validator)),
      repository_(std::move(limits)),
      executor_(repository_, logger, std::move(handler)),
      pool_(std::move(pool)) {}

TaskManager::SubmissionGuard::SubmissionGuard(TaskManager& manager) noexcept
    : manager_(manager) {}

TaskManager::SubmissionGuard::~SubmissionGuard() {
    manager_.finish_submission();
}

TaskManager::~TaskManager() {
    const auto stopped = shutdown();
    if (!stopped) {
        std::terminate();
    }
}

Result<TaskId> TaskManager::submit(const TaskRequest& request) {
    auto outcome = submit_with_outcome(request);
    return std::move(outcome.result);
}

TaskSubmitOutcome TaskManager::submit_with_outcome(
    const TaskRequest& request) {
    auto admitted = begin_submission();
    if (!admitted) {
        return rejected(
            TaskSubmitFailure::NotAccepting,
            std::move(admitted).error());
    }
    const SubmissionGuard submission{*this};
    return submit_admitted(request);
}

TaskSubmitOutcome TaskManager::submit_admitted(const TaskRequest& request) {
    auto valid_request = repository_.limits().validate_request(request);
    if (!valid_request) {
        return rejected(
            TaskSubmitFailure::InvalidRequest,
            std::move(valid_request).error());
    }

    if (validator_) {
        try {
            auto validated = validator_(request);
            if (!validated) {
                return rejected(
                    TaskSubmitFailure::ValidationRejected,
                    std::move(validated).error());
            }
        } catch (const std::exception&) {
            return rejected(
                TaskSubmitFailure::InternalFailure,
                make_error(
                    ErrorCode::InternalError,
                    "task validation failed"));
        } catch (...) {
            return rejected(
                TaskSubmitFailure::InternalFailure,
                make_error(
                    ErrorCode::InternalError,
                    "task validation failed"));
        }
    }

    auto created = repository_.create_queued(request);
    if (!created) {
        const auto failure =
            created.error().code == ErrorCode::ResourceExhausted
            ? TaskSubmitFailure::RepositoryCapacity
            : TaskSubmitFailure::InternalFailure;
        return rejected(failure, std::move(created).error());
    }
    const TaskId id = created.value();

    try {
        TaskRequest work_request = request;
        BoundedThreadPool::WorkItem work{
            [executor = &executor_, id, request = std::move(work_request)] {
                executor->execute(id, request);
            }};
        auto submitted = pool_->try_submit(std::move(work));
        if (submitted) {
            return accepted(id);
        }

        const auto rolled_back = repository_.rollback_queued(id);
        if (!rolled_back) {
            return rejected(
                TaskSubmitFailure::InternalFailure,
                make_error(
                    ErrorCode::InternalError,
                    "task queue rejection rollback failed"));
        }
        const auto failure =
            submitted.error().code == ErrorCode::ResourceExhausted
            ? TaskSubmitFailure::QueueCapacity
            : (submitted.error().code == ErrorCode::InvalidState
                   ? TaskSubmitFailure::NotAccepting
                   : TaskSubmitFailure::InternalFailure);
        return rejected(failure, std::move(submitted).error());
    } catch (const std::bad_alloc&) {
        const auto rolled_back = repository_.rollback_queued(id);
        if (!rolled_back) {
            return rejected(
                TaskSubmitFailure::InternalFailure,
                make_error(
                    ErrorCode::InternalError,
                    "task allocation rollback failed"));
        }
        return rejected(
            TaskSubmitFailure::ResourceFailure,
            make_error(
                ErrorCode::ResourceExhausted,
                "unable to allocate submitted task work item"));
    } catch (const std::length_error&) {
        const auto rolled_back = repository_.rollback_queued(id);
        if (!rolled_back) {
            return rejected(
                TaskSubmitFailure::InternalFailure,
                make_error(
                    ErrorCode::InternalError,
                    "task length failure rollback failed"));
        }
        return rejected(
            TaskSubmitFailure::ResourceFailure,
            make_error(
                ErrorCode::ResourceExhausted,
                "submitted task exceeds the platform size limit"));
    } catch (const std::exception&) {
        const auto rolled_back = repository_.rollback_queued(id);
        if (!rolled_back) {
            return rejected(
                TaskSubmitFailure::InternalFailure,
                make_error(
                    ErrorCode::InternalError,
                    "task submission rollback failed"));
        }
        return rejected(
            TaskSubmitFailure::InternalFailure,
            make_error(
                ErrorCode::InternalError,
                "task submission failed"));
    } catch (...) {
        const auto rolled_back = repository_.rollback_queued(id);
        if (!rolled_back) {
            return rejected(
                TaskSubmitFailure::InternalFailure,
                make_error(
                    ErrorCode::InternalError,
                    "unknown task submission rollback failed"));
        }
        return rejected(
            TaskSubmitFailure::InternalFailure,
            make_error(
                ErrorCode::InternalError,
                "task submission failed"));
    }
}

Result<TaskSnapshot> TaskManager::get_snapshot(const TaskId id) const {
    return repository_.get_snapshot(id);
}

Result<TransitionOutcome> TaskManager::mark_timed_out(
    const TaskId id,
    const Error& error) {
    return repository_.mark_timed_out(id, error);
}

Result<void> TaskManager::erase_terminal(const TaskId id) {
    return repository_.erase_terminal(id);
}

Result<void> TaskManager::shutdown() {
    if (pool_->is_worker_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "a task worker cannot shut down its task manager"));
    }

    {
        std::unique_lock<std::mutex> lock(admission_mutex_);
        accepting_ = false;
        submissions_finished_.notify_all();
        submissions_finished_.wait(
            lock,
            [this] { return in_flight_submissions_ == 0; });
    }
    return pool_->shutdown();
}

bool TaskManager::accepting() const {
    std::lock_guard<std::mutex> lock(admission_mutex_);
    return accepting_ && pool_->accepting();
}

bool TaskManager::stopped() const {
    return pool_->stopped();
}

std::size_t TaskManager::repository_size() const {
    return repository_.size();
}

std::size_t TaskManager::pending_count() const {
    return pool_->pending_count();
}

std::size_t TaskManager::task_exception_count() const noexcept {
    return pool_->task_exception_count();
}

std::size_t TaskManager::late_completion_count() const noexcept {
    return executor_.late_completion_count();
}

std::size_t TaskManager::handler_exception_count() const noexcept {
    return executor_.handler_exception_count();
}

std::size_t TaskManager::logger_failure_count() const noexcept {
    return executor_.logger_failure_count();
}

Result<void> TaskManager::begin_submission() {
    std::lock_guard<std::mutex> lock(admission_mutex_);
    if (!accepting_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "task manager is not accepting submissions"));
    }
    ++in_flight_submissions_;
    return Result<void>::success();
}

void TaskManager::finish_submission() noexcept {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(admission_mutex_);
        if (in_flight_submissions_ == 0) {
            std::terminate();
        }
        --in_flight_submissions_;
        notify = in_flight_submissions_ == 0;
    }
    if (notify) {
        submissions_finished_.notify_all();
    }
}

}  // namespace iaisf::task
