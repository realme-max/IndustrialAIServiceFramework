#include "iaisf/application/application_executor.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::application {

ApplicationTaskManager::ApplicationTaskManager(const std::size_t capacity)
    : capacity_(capacity), worker_(&ApplicationTaskManager::run, this) {}

Result<std::unique_ptr<ApplicationTaskManager>>
ApplicationTaskManager::create(const std::size_t capacity) {
    if (capacity == 0U) {
        return Result<std::unique_ptr<ApplicationTaskManager>>::failure(
            make_error(ErrorCode::InvalidArgument,
                       "application task queue capacity must be positive"));
    }
    try {
        return Result<std::unique_ptr<ApplicationTaskManager>>::success(
            std::unique_ptr<ApplicationTaskManager>{
                new ApplicationTaskManager{capacity}});
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<ApplicationTaskManager>>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "unable to allocate application task manager"));
    } catch (const std::system_error&) {
        return Result<std::unique_ptr<ApplicationTaskManager>>::failure(
            make_error(ErrorCode::InternalError,
                       "unable to start application task manager"));
    }
}

ApplicationTaskManager::~ApplicationTaskManager() noexcept {
    stop_admission();
    const auto result = shutdown();
    if (!result) {
        std::terminate();
    }
}

Result<void> ApplicationTaskManager::submit(std::function<void()> task) {
    if (!task) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "application task is empty"));
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidState,
                           "application task manager is stopping"));
        }
        if (queue_.size() >= capacity_) {
            return Result<void>::failure(
                make_error(ErrorCode::ResourceExhausted,
                           "application task queue is full"));
        }
        queue_.push_back(std::move(task));
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to enqueue application task"));
    }
    condition_.notify_one();
    return Result<void>::success();
}

void ApplicationTaskManager::stop_admission() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        stopping_ = true;
    }
    condition_.notify_all();
}

Result<void> ApplicationTaskManager::shutdown() {
    stop_admission();
    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidState,
                "application task manager cannot join its worker"));
        }
        worker_.join();
    }
    return Result<void>::success();
}

bool ApplicationTaskManager::stopped() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_ && !worker_.joinable();
}

std::size_t ApplicationTaskManager::pending() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void ApplicationTaskManager::run() noexcept {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });
            if (queue_.empty() && stopping_) {
                stopped_ = true;
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            task();
        } catch (...) {
            // The executor owns terminal-state conversion. A worker exception
            // must never terminate the process or the queue thread.
        }
    }
}

ApplicationExecutor::ApplicationExecutor(
    IApplicationJobRepository& repository,
    Ptv2WeldInspectionAdapter* const inspection_adapter,
    WeldAgentWeldingGuidanceAdapter* const guidance_adapter,
    const IApplicationJobClock& clock,
    std::unique_ptr<ApplicationTaskManager> tasks) noexcept
    : repository_(repository),
      inspection_adapter_(inspection_adapter),
      guidance_adapter_(guidance_adapter),
      clock_(clock),
      tasks_(std::move(tasks)) {}

Result<std::unique_ptr<ApplicationExecutor>> ApplicationExecutor::create(
    IApplicationJobRepository& repository,
    Ptv2WeldInspectionAdapter* const inspection_adapter,
    WeldAgentWeldingGuidanceAdapter* const guidance_adapter,
    const IApplicationJobClock& clock,
    const std::size_t queue_capacity) {
    if (inspection_adapter == nullptr && guidance_adapter == nullptr) {
        return Result<std::unique_ptr<ApplicationExecutor>>::failure(
            make_error(ErrorCode::InvalidArgument,
                       "application executor has no adapters"));
    }
    auto tasks = ApplicationTaskManager::create(queue_capacity);
    if (!tasks) {
        return Result<std::unique_ptr<ApplicationExecutor>>::failure(
            std::move(tasks).error());
    }
    try {
        return Result<std::unique_ptr<ApplicationExecutor>>::success(
            std::unique_ptr<ApplicationExecutor>{new ApplicationExecutor{
                repository, inspection_adapter, guidance_adapter, clock,
                std::move(tasks).value()}});
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<ApplicationExecutor>>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "unable to allocate application executor"));
    }
}

ApplicationExecutor::~ApplicationExecutor() noexcept {
    stop_admission();
    const auto result = shutdown();
    if (!result) {
        std::terminate();
    }
}

Result<void> ApplicationExecutor::submit(const ApplicationJobId& job_id) {
    if (!job_id.valid()) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "application job id is invalid"));
    }
    if (!accepting_.load(std::memory_order_acquire)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState, "application executor is stopping"));
    }
    auto submitted = tasks_->submit([this, job_id] { execute(job_id); });
    if (!submitted) {
        return submitted;
    }
    return Result<void>::success();
}

void ApplicationExecutor::stop_admission() noexcept {
    accepting_.store(false, std::memory_order_release);
    tasks_->stop_admission();
}

Result<void> ApplicationExecutor::shutdown() {
    stop_admission();
    return tasks_->shutdown();
}

bool ApplicationExecutor::stopped() const noexcept {
    return tasks_->stopped();
}

Result<ApplicationJobTimePoint> ApplicationExecutor::effective_timestamp(
    const ApplicationJobSnapshot& snapshot) const {
    auto now = clock_.now();
    if (!now) {
        return Result<ApplicationJobTimePoint>::failure(std::move(now).error());
    }
    return Result<ApplicationJobTimePoint>::success(
        std::max(now.value(), snapshot.updated_at()));
}

ApplicationJobTimePoint ApplicationExecutor::failure_timestamp(
    const ApplicationJobSnapshot& snapshot) const {
    auto now = clock_.now();
    if (!now) {
        return snapshot.updated_at();
    }
    return std::max(now.value(), snapshot.updated_at());
}

Result<void> ApplicationExecutor::fail_running(
    const ApplicationJobSnapshot& snapshot) {
    auto current_result = repository_.get(snapshot.job_id(), snapshot.application());
    if (!current_result) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "unable to read application job for failure transition"));
    }

    auto current = std::move(current_result).value();
    if (is_terminal(current.state())) {
        return Result<void>::success();
    }

    auto transitioned = repository_.transition(
        current.job_id(), current.application(), current.version(),
        ApplicationJobState::Failed, failure_timestamp(current));
    if (transitioned) {
        return Result<void>::success();
    }
    if (transitioned.error().category !=
        ApplicationRepositoryFailure::VersionConflict) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "application job failure transition was rejected"));
    }

    auto refreshed_result =
        repository_.get(snapshot.job_id(), snapshot.application());
    if (!refreshed_result) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "unable to reread application job after failure conflict"));
    }
    auto refreshed = std::move(refreshed_result).value();
    if (is_terminal(refreshed.state())) {
        return Result<void>::success();
    }
    auto retried = repository_.transition(
        refreshed.job_id(), refreshed.application(), refreshed.version(),
        ApplicationJobState::Failed, failure_timestamp(refreshed));
    if (retried) {
        return Result<void>::success();
    }
    return Result<void>::failure(make_error(
        ErrorCode::InternalError,
        "application job failure transition remained unresolved"));
}

void ApplicationExecutor::execute(const ApplicationJobId& job_id) noexcept {
    const auto text = job_id.value();
    IndustrialApplication application;
    if (text.rfind("wi_", 0U) == 0U) {
        application = IndustrialApplication::WeldInspection;
    } else if (text.rfind("wg_", 0U) == 0U) {
        application = IndustrialApplication::WeldingGuidance;
    } else {
        return;
    }
    auto snapshot_result = repository_.get(job_id, application);
    if (!snapshot_result) {
        return;
    }
    auto snapshot = std::move(snapshot_result).value();
    auto dispatching_at = effective_timestamp(snapshot);
    if (!dispatching_at) {
        const auto failed = fail_running(snapshot);
        if (!failed) {
            return;
        }
        return;
    }
    auto dispatching = repository_.transition(
        job_id, application, snapshot.version(),
        ApplicationJobState::Dispatching, dispatching_at.value());
    if (!dispatching) {
        return;
    }
    snapshot = std::move(dispatching).value();
    auto running_at = effective_timestamp(snapshot);
    if (!running_at) {
        const auto failed = fail_running(snapshot);
        if (!failed) {
            return;
        }
        return;
    }
    auto running = repository_.transition(
        job_id, application, snapshot.version(), ApplicationJobState::Running,
        running_at.value());
    if (!running) {
        return;
    }
    snapshot = std::move(running).value();

    try {
        Result<ApplicationExecutionResult> result =
            Result<ApplicationExecutionResult>::failure(make_error(
                ErrorCode::InternalError, "application adapter unavailable"));
        if (application == IndustrialApplication::WeldInspection &&
            inspection_adapter_ != nullptr) {
            result = inspection_adapter_->execute(snapshot);
        } else if (application == IndustrialApplication::WeldingGuidance &&
                   guidance_adapter_ != nullptr) {
            result = guidance_adapter_->execute(snapshot);
        }
        if (!result) {
            const auto failed = fail_running(snapshot);
            if (!failed) {
                return;
            }
            return;
        }
        auto completed_at = effective_timestamp(snapshot);
        if (!completed_at) {
            const auto failed = fail_running(snapshot);
            if (!failed) {
                return;
            }
            return;
        }
        auto completed = repository_.complete(
            job_id, application, snapshot.version(),
            std::get_if<WeldingGuidanceResult>(&result.value()) != nullptr &&
                    std::get<WeldingGuidanceResult>(result.value()).disposition ==
                        GuidanceResultDisposition::WaitingHuman
                ? ApplicationJobState::WaitingHuman
                : ApplicationJobState::Succeeded,
            std::move(result).value(), completed_at.value());
        if (!completed) {
            const auto failed = fail_running(snapshot);
            if (!failed) {
                return;
            }
        }
    } catch (...) {
        const auto failed = fail_running(snapshot);
        if (!failed) {
            return;
        }
    }
}

}  // namespace iaisf::application
