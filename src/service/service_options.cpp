#include "iaisf/service/service_options.hpp"

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

#include "iaisf/config/app_config.hpp"
#include "iaisf/core/error.hpp"

namespace iaisf::service {
namespace {

bool can_add(const std::size_t left, const std::size_t right) noexcept {
    return right <= std::numeric_limits<std::size_t>::max() - left;
}

Result<void> validate_cross_limits(
    const net::tcp::TcpServerOptions& tcp,
    const http::HttpLimits& http,
    const task::ThreadPoolOptions pool,
    const task::TaskLimits& task,
    const plugin::PluginLimits& plugin,
    const api::TaskApiLimits& api_options,
    const bool metrics_enabled,
    const bool diagnostics_enabled,
    const bool applications_enabled) {
    auto pool_valid = task::BoundedThreadPool::validate_options(pool);
    if (!pool_valid) {
        return pool_valid;
    }
    const std::size_t required_routes = 5U +
        (metrics_enabled ? 1U : 0U) + (diagnostics_enabled ? 1U : 0U);
    const std::size_t application_routes = applications_enabled ? 6U : 0U;
    if (http.max_routes() < required_routes ||
        application_routes > http.max_routes() - required_routes) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP route capacity cannot represent the required service routes"));
    }

    auto api_valid = api::validate_task_api_capacity(
        task,
        plugin,
        http,
        api_options);
    if (!api_valid) {
        return api_valid;
    }
    if (!can_add(http.max_header_bytes(), http.max_body_bytes()) ||
        http.max_header_bytes() + http.max_body_bytes() >
            tcp.input_maximum_capacity()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TCP input capacity cannot carry the maximum HTTP request"));
    }
    if (!can_add(
            http.max_header_bytes(),
            http.max_response_body_bytes()) ||
        http.max_header_bytes() + http.max_response_body_bytes() >
            tcp.output_maximum_capacity()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TCP output capacity cannot carry the maximum HTTP response"));
    }
    return Result<void>::success();
}

}  // namespace

Result<ServiceOptions> ServiceOptions::create(
    net::tcp::TcpServerOptions tcp_options,
    http::HttpLimits http_limits,
    const task::ThreadPoolOptions pool_options,
    task::TaskLimits task_limits,
    plugin::PluginLimits plugin_limits,
    api::TaskApiLimits api_limits,
    const bool enable_echo,
    const bool enable_mock_vision,
    const std::optional<std::chrono::milliseconds> http_header_timeout,
    const std::optional<std::chrono::milliseconds> http_body_timeout,
    const bool metrics_enabled,
    std::string metrics_endpoint,
    const bool diagnostics_enabled,
    std::string diagnostics_endpoint,
    DynamicPluginOptions dynamic_plugins,
    ApplicationRuntimeOptions applications) {
    auto valid = validate_cross_limits(
        tcp_options,
        http_limits,
        pool_options,
        task_limits,
        plugin_limits,
        api_limits,
        metrics_enabled, diagnostics_enabled, applications.enabled);
    if (!valid) {
        return Result<ServiceOptions>::failure(std::move(valid).error());
    }
    if ((http_header_timeout.has_value() &&
         *http_header_timeout <= std::chrono::milliseconds::zero()) ||
        (http_body_timeout.has_value() &&
         *http_body_timeout <= std::chrono::milliseconds::zero())) {
        return Result<ServiceOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP timeout values must be positive when enabled"));
    }
    if (metrics_endpoint.empty() ||
        metrics_endpoint.size() > kMaxMetricsEndpointBytes ||
        metrics_endpoint.front() != '/' ||
        metrics_endpoint.find_first_of("?#\\") != std::string::npos) {
        return Result<ServiceOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "metrics endpoint must be a valid path of at most 128 bytes"));
    }
    if (diagnostics_endpoint.empty() ||
        diagnostics_endpoint.size() > kMaxDiagnosticsEndpointBytes ||
        diagnostics_endpoint.front() != '/' ||
        diagnostics_endpoint.find_first_of("?#\\") != std::string::npos) {
        return Result<ServiceOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "diagnostics endpoint must be a valid path of at most 128 bytes"));
    }
    const auto reserved = [](const std::string& endpoint) {
        return endpoint == "/health" || endpoint == "/ready" ||
               endpoint == "/version" || endpoint == "/v1/tasks" ||
               endpoint.rfind("/v1/tasks/", 0U) == 0U;
    };
    if (reserved(diagnostics_endpoint) ||
        (metrics_enabled && diagnostics_endpoint == metrics_endpoint)) {
        return Result<ServiceOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "diagnostics endpoint conflicts with a service route"));
    }
    if (dynamic_plugins.root.empty() || dynamic_plugins.max_modules == 0U ||
        dynamic_plugins.modules.size() > dynamic_plugins.max_modules) {
        return Result<ServiceOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "dynamic plugin options are outside supported ranges"));
    }
    if (applications.enabled) {
        if (applications.repository_capacity == 0U ||
            applications.repository_capacity > kMaxApplicationRepositoryCapacity ||
            applications.queue_capacity == 0U ||
            applications.queue_capacity > kMaxApplicationQueueCapacity ||
            applications.artifact_root.empty() || applications.scratch_root.empty() ||
            applications.output_root.empty() || applications.ptv2.executable.empty() ||
            applications.ptv2.engine.empty() || applications.ptv2.plugin.empty() ||
            applications.weld_agent.python_executable.empty() ||
            applications.weld_agent.orchestrator.empty() ||
            applications.weld_agent.tool_config.empty() ||
            applications.weld_agent.project_root.empty() ||
            applications.ptv2.timeout <= std::chrono::milliseconds::zero() ||
            applications.weld_agent.timeout <= std::chrono::milliseconds::zero()) {
            return Result<ServiceOptions>::failure(make_error(
                ErrorCode::InvalidArgument,
                "application runtime options are outside supported ranges"));
        }
    }
    const std::size_t static_plugin_count =
        (enable_echo ? 1U : 0U) + (enable_mock_vision ? 1U : 0U);
    if (static_plugin_count > plugin_limits.max_plugins() ||
        (dynamic_plugins.enabled &&
         dynamic_plugins.modules.size() >
             plugin_limits.max_plugins() - static_plugin_count)) {
        return Result<ServiceOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "enabled static and dynamic plugins exceed plugin capacity"));
    }
    return Result<ServiceOptions>::success(ServiceOptions{
        std::move(tcp_options),
        std::move(http_limits),
        pool_options,
        std::move(task_limits),
        std::move(plugin_limits),
        std::move(api_limits),
        enable_echo,
        enable_mock_vision,
        http_header_timeout,
        http_body_timeout,
        metrics_enabled,
        std::move(metrics_endpoint),
        diagnostics_enabled,
        std::move(diagnostics_endpoint),
        std::move(dynamic_plugins), std::move(applications)});
}

Result<ServiceOptions> ServiceOptions::defaults() {
    auto tcp = net::tcp::TcpServerOptions::create(
        128,
        1024,
        4096,
        2 * 1024 * 1024,
        4096,
        512 * 1024,
        2 * 1024 * 1024);
    auto task_limits_result = task::TaskLimits::create(
        100000,
        256,
        512 * 1024,
        512 * 1024,
        1024,
        64,
        100000,
        512 * 1024);
    auto plugin_limits_result = plugin::PluginLimits::create(
        128,
        128,
        128,
        64,
        1024,
        1024,
        512 * 1024,
        512 * 1024,
        64,
        100000,
        512 * 1024,
        64,
        128);
    if (!tcp || !task_limits_result || !plugin_limits_result) {
        return Result<ServiceOptions>::failure(make_error(
            ErrorCode::InternalError,
            "built-in service defaults are invalid"));
    }
    const unsigned int detected_threads = std::thread::hardware_concurrency();
    const std::size_t worker_threads = std::clamp<std::size_t>(
        detected_threads == 0U ? 1U : static_cast<std::size_t>(detected_threads),
        1U,
        task::BoundedThreadPool::kMaximumWorkerCount);
    return create(
        std::move(tcp).value(),
        http::HttpLimits::defaults(),
        task::ThreadPoolOptions{worker_threads, 1024U},
        std::move(task_limits_result).value(),
        std::move(plugin_limits_result).value(),
        api::TaskApiLimits::defaults());
}

ServiceOptions::ServiceOptions(
    net::tcp::TcpServerOptions tcp_options,
    http::HttpLimits http_limits,
    const task::ThreadPoolOptions pool_options,
    task::TaskLimits task_limits,
    plugin::PluginLimits plugin_limits,
    api::TaskApiLimits api_limits,
    const bool enable_echo,
    const bool enable_mock_vision,
    const std::optional<std::chrono::milliseconds> http_header_timeout,
    const std::optional<std::chrono::milliseconds> http_body_timeout,
    const bool metrics_enabled,
    std::string metrics_endpoint,
    const bool diagnostics_enabled,
    std::string diagnostics_endpoint,
    DynamicPluginOptions dynamic_plugins,
    ApplicationRuntimeOptions applications) noexcept
    : tcp_options_(std::move(tcp_options)),
      http_limits_(std::move(http_limits)),
      pool_options_(pool_options),
      task_limits_(std::move(task_limits)),
      plugin_limits_(std::move(plugin_limits)),
      api_limits_(std::move(api_limits)),
      enable_echo_(enable_echo),
      enable_mock_vision_(enable_mock_vision),
      http_header_timeout_(http_header_timeout),
      http_body_timeout_(http_body_timeout),
      metrics_enabled_(metrics_enabled),
      metrics_endpoint_(std::move(metrics_endpoint)),
      diagnostics_enabled_(diagnostics_enabled),
      diagnostics_endpoint_(std::move(diagnostics_endpoint)),
      dynamic_plugins_(std::move(dynamic_plugins)),
      applications_(std::move(applications)) {}

const net::tcp::TcpServerOptions& ServiceOptions::tcp_options() const noexcept {
    return tcp_options_;
}
const http::HttpLimits& ServiceOptions::http_limits() const noexcept {
    return http_limits_;
}
task::ThreadPoolOptions ServiceOptions::pool_options() const noexcept {
    return pool_options_;
}
const task::TaskLimits& ServiceOptions::task_limits() const noexcept {
    return task_limits_;
}
const plugin::PluginLimits& ServiceOptions::plugin_limits() const noexcept {
    return plugin_limits_;
}
const api::TaskApiLimits& ServiceOptions::api_limits() const noexcept {
    return api_limits_;
}
bool ServiceOptions::enable_echo() const noexcept {
    return enable_echo_;
}
bool ServiceOptions::enable_mock_vision() const noexcept {
    return enable_mock_vision_;
}

std::optional<std::chrono::milliseconds>
ServiceOptions::http_header_timeout() const noexcept {
    return http_header_timeout_;
}

std::optional<std::chrono::milliseconds>
ServiceOptions::http_body_timeout() const noexcept {
    return http_body_timeout_;
}

bool ServiceOptions::metrics_enabled() const noexcept {
    return metrics_enabled_;
}

const std::string& ServiceOptions::metrics_endpoint() const noexcept {
    return metrics_endpoint_;
}

bool ServiceOptions::diagnostics_enabled() const noexcept {
    return diagnostics_enabled_;
}

const std::string& ServiceOptions::diagnostics_endpoint() const noexcept {
    return diagnostics_endpoint_;
}

const DynamicPluginOptions& ServiceOptions::dynamic_plugins() const noexcept {
    return dynamic_plugins_;
}

const ApplicationRuntimeOptions& ServiceOptions::applications() const noexcept {
    return applications_;
}

}  // namespace iaisf::service
