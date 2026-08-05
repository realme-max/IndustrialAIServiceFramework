#include "iaisf/task/task_executor.hpp"

#include <exception>
#include <memory>
#include <utility>

namespace iaisf::task {
namespace {

template <typename Metric, typename Create, typename Get>
std::shared_ptr<Metric> metric_or_existing(
    Create&& create,
    Get&& get) noexcept {
    try {
        auto created = create();
        if (created) {
            return created.value();
        }
        auto existing = get();
        if (existing) {
            return existing.value();
        }
    } catch (...) {
        // Metrics are observational and never affect task execution.
    }
    return {};
}

class RunningMetricGuard final {
public:
    explicit RunningMetricGuard(const std::shared_ptr<Gauge>& metric) noexcept
        : metric_(metric) {}

    RunningMetricGuard(const RunningMetricGuard&) = delete;
    RunningMetricGuard& operator=(const RunningMetricGuard&) = delete;

    void start() noexcept {
        active_ = true;
        if (metric_) {
            metric_->increment();
        }
    }

    ~RunningMetricGuard() {
        if (active_ && metric_) {
            metric_->decrement();
        }
    }

private:
    std::shared_ptr<Gauge> metric_;
    bool active_{false};
};

}  // namespace

TaskExecutor::TaskExecutor(
    TaskRepository& repository,
    ILogger& logger,
    TaskHandler handler,
    MetricsRegistry* const metrics)
    : repository_(repository),
      logger_(logger),
      handler_(std::move(handler)),
      metrics_(metrics) {
    if (metrics_ == nullptr) {
        return;
    }
    completed_metric_ = metric_or_existing<Counter>(
        [this] { return metrics_->create_counter("tasks_completed_total"); },
        [this] { return metrics_->get_counter("tasks_completed_total"); });
    failed_metric_ = metric_or_existing<Counter>(
        [this] { return metrics_->create_counter("tasks_failed_total"); },
        [this] { return metrics_->get_counter("tasks_failed_total"); });
    running_metric_ = metric_or_existing<Gauge>(
        [this] { return metrics_->create_gauge("tasks_running"); },
        [this] { return metrics_->get_gauge("tasks_running"); });
}

void TaskExecutor::execute(const TaskId id, const TaskRequest& request) noexcept {
    RunningMetricGuard running_guard{running_metric_};
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
        running_guard.start();

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
            observe_transition(completed, true);
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
        observe_transition(completed, false);
    } catch (...) {
        safe_log("task failure transition raised an internal exception");
    }
}

void TaskExecutor::observe_transition(
    const Result<TransitionOutcome>& transition,
    const bool success) noexcept {
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
        return;
    }
    if (success) {
        if (completed_metric_) {
            completed_metric_->increment();
        }
    } else if (failed_metric_) {
        failed_metric_->increment();
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
