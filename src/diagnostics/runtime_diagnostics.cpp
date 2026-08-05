#include "iaisf/diagnostics/runtime_diagnostics.hpp"

#include <new>
#include <utility>

#include <nlohmann/json.hpp>

namespace iaisf::diagnostics {
namespace {

using Json = nlohmann::json;

const char* health_phase_name(const health::HealthPhase phase) noexcept {
    return health::to_string(phase);
}

const char* logger_state_name(const LogDiagnosticsState state) noexcept {
    switch (state) {
    case LogDiagnosticsState::Running:
        return "running";
    case LogDiagnosticsState::Draining:
        return "draining";
    case LogDiagnosticsState::Stopped:
        return "stopped";
    }
    return "stopped";
}

} // namespace

Result<std::shared_ptr<RuntimeDiagnostics>> RuntimeDiagnostics::create(
    std::shared_ptr<const health::HealthChecker> health_checker,
    const task::TaskManager& task_manager,
    const MetricsRegistry& metrics,
    const ILogDiagnostics* const logger) {
    if (!health_checker) {
        return Result<std::shared_ptr<RuntimeDiagnostics>>::failure(
            make_error(ErrorCode::InvalidArgument,
                       "runtime diagnostics requires a health checker"));
    }
    try {
        return Result<std::shared_ptr<RuntimeDiagnostics>>::success(
            std::make_shared<RuntimeDiagnostics>(std::move(health_checker),
                                                  task_manager, metrics,
                                                  logger));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<RuntimeDiagnostics>>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "runtime diagnostics allocation failed"));
    } catch (...) {
        return Result<std::shared_ptr<RuntimeDiagnostics>>::failure(
            make_error(ErrorCode::InternalError,
                       "runtime diagnostics creation failed"));
    }
}

RuntimeDiagnostics::RuntimeDiagnostics(
    std::shared_ptr<const health::HealthChecker> health_checker,
    const task::TaskManager& task_manager,
    const MetricsRegistry& metrics,
    const ILogDiagnostics* const logger) noexcept
    : health_checker_(std::move(health_checker)),
      task_manager_(task_manager),
      metrics_(metrics),
      logger_(logger) {}

Result<RuntimeDiagnosticsSnapshot> RuntimeDiagnostics::snapshot() const {
    try {
        RuntimeDiagnosticsSnapshot result;
        result.health = health_checker_->snapshot();
        result.tasks.accepting = task_manager_.accepting();
        result.tasks.stopped = task_manager_.stopped();
        result.tasks.repository_size = task_manager_.repository_size();
        result.tasks.pending_count = task_manager_.pending_count();
        result.tasks.task_exceptions = task_manager_.task_exception_count();
        result.tasks.late_completions = task_manager_.late_completion_count();
        result.tasks.handler_exceptions = task_manager_.handler_exception_count();
        result.tasks.logger_failures = task_manager_.logger_failure_count();
        if (logger_ != nullptr) {
            const auto log = logger_->diagnostics_snapshot();
            result.logger.available = true;
            result.logger.state = log.state;
            result.logger.accepted = log.accepted;
            result.logger.filtered = log.filtered;
            result.logger.dropped = log.dropped;
            result.logger.rejected_after_shutdown = log.rejected_after_shutdown;
            result.logger.sink_failures = log.sink_failures;
        }
        auto metrics = metrics_.snapshot();
        if (!metrics) {
            return Result<RuntimeDiagnosticsSnapshot>::failure(
                std::move(metrics).error());
        }
        result.metrics = std::move(metrics).value();
        return Result<RuntimeDiagnosticsSnapshot>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<RuntimeDiagnosticsSnapshot>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "runtime diagnostics snapshot allocation failed"));
    } catch (...) {
        return Result<RuntimeDiagnosticsSnapshot>::failure(
            make_error(ErrorCode::InternalError,
                       "runtime diagnostics snapshot failed"));
    }
}

Result<std::string> to_json(const RuntimeDiagnosticsSnapshot& snapshot,
                            const std::size_t maximum_bytes) {
    if (maximum_bytes == 0U) {
        return Result<std::string>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "diagnostics response limit is zero"));
    }
    try {
        Json root;
        root["schema_version"] = 1;
        root["service"] = {{"phase", health_phase_name(snapshot.health.phase)},
                             {"live", snapshot.health.live},
                             {"ready", snapshot.health.ready}};
        root["tasks"] = {{"accepting", snapshot.tasks.accepting},
                           {"stopped", snapshot.tasks.stopped},
                           {"repository_size", snapshot.tasks.repository_size},
                           {"pending_count", snapshot.tasks.pending_count},
                           {"task_exceptions", snapshot.tasks.task_exceptions},
                           {"late_completions", snapshot.tasks.late_completions},
                           {"handler_exceptions", snapshot.tasks.handler_exceptions},
                           {"logger_failures", snapshot.tasks.logger_failures}};
        root["logger"] = {{"available", snapshot.logger.available},
                            {"state", logger_state_name(snapshot.logger.state)},
                            {"accepted", snapshot.logger.accepted},
                            {"filtered", snapshot.logger.filtered},
                            {"dropped", snapshot.logger.dropped},
                            {"rejected_after_shutdown",
                             snapshot.logger.rejected_after_shutdown},
                            {"sink_failures", snapshot.logger.sink_failures}};
        Json counters = Json::array();
        for (const auto& value : snapshot.metrics.counters) {
            counters.push_back({{"name", value.name}, {"value", value.value}});
        }
        Json gauges = Json::array();
        for (const auto& value : snapshot.metrics.gauges) {
            gauges.push_back({{"name", value.name}, {"value", value.value}});
        }
        Json histograms = Json::array();
        for (const auto& value : snapshot.metrics.histograms) {
            histograms.push_back({{"name", value.name},
                                  {"count", value.count},
                                  {"sum", value.sum},
                                  {"min", value.min},
                                  {"max", value.max}});
        }
        root["metrics"] = {{"counters", std::move(counters)},
                            {"gauges", std::move(gauges)},
                            {"histograms", std::move(histograms)}};
        auto encoded = root.dump();
        if (encoded.size() > maximum_bytes) {
            return Result<std::string>::failure(
                make_error(ErrorCode::ResourceExhausted,
                           "diagnostics response exceeds configured limit"));
        }
        return Result<std::string>::success(std::move(encoded));
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "diagnostics response allocation failed"));
    } catch (...) {
        return Result<std::string>::failure(
            make_error(ErrorCode::InternalError,
                       "diagnostics response serialization failed"));
    }
}

} // namespace iaisf::diagnostics
