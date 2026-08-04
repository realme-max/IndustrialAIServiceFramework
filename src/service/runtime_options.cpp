#include "iaisf/service/runtime_options.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

#include "iaisf/api/task_api_limits.hpp"
#include "iaisf/core/error.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/net/tcp/tcp_server.hpp"
#include "iaisf/plugin/plugin_limits.hpp"
#include "iaisf/task/task_limits.hpp"
#include "iaisf/task/thread_pool.hpp"

namespace iaisf::service {
namespace {

template <typename T> Result<T> config_failure(const Error &source) {
    return Result<T>::failure(
        make_error(ErrorCode::ConfigError, source.message));
}

std::optional<std::chrono::milliseconds>
as_timeout(const std::optional<std::int64_t> milliseconds) {
    if (!milliseconds.has_value()) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{*milliseconds};
}

} // namespace

RuntimeOptions::RuntimeOptions(const std::size_t reactor_max_events,
                               const std::size_t pending_callback_capacity,
                               const net::TimerQueueOptions timer_options,
                               net::tcp::Ipv4Endpoint bind_endpoint,
                               ServiceOptions service_options) noexcept
    : reactor_max_events_(reactor_max_events),
      pending_callback_capacity_(pending_callback_capacity),
      timer_options_(timer_options), bind_endpoint_(std::move(bind_endpoint)),
      service_options_(std::move(service_options)) {}

std::size_t RuntimeOptions::reactor_max_events() const noexcept {
    return reactor_max_events_;
}

std::size_t RuntimeOptions::pending_callback_capacity() const noexcept {
    return pending_callback_capacity_;
}

net::TimerQueueOptions RuntimeOptions::timer_options() const noexcept {
    return timer_options_;
}

const net::tcp::Ipv4Endpoint &RuntimeOptions::bind_endpoint() const noexcept {
    return bind_endpoint_;
}

const ServiceOptions &RuntimeOptions::service_options() const noexcept {
    return service_options_;
}

Result<RuntimeOptions> make_runtime_options(const AppConfig &config) {
    auto app_valid = validate_app_config(config);
    if (!app_valid) {
        return config_failure<RuntimeOptions>(app_valid.error());
    }

    auto endpoint = net::tcp::Ipv4Endpoint::from_string(config.server.host,
                                                        config.server.port);
    if (!endpoint) {
        return config_failure<RuntimeOptions>(endpoint.error());
    }
    auto timers =
        net::TimerQueueOptions::create(config.server.reactor.max_timers);
    if (!timers) {
        return config_failure<RuntimeOptions>(timers.error());
    }
    auto tcp = net::tcp::TcpServerOptions::create(
        config.server.tcp.listen_backlog, config.server.tcp.max_connections,
        config.server.tcp.input_initial_capacity_bytes,
        config.server.tcp.input_maximum_capacity_bytes,
        config.server.tcp.output_initial_capacity_bytes,
        config.server.tcp.output_high_water_mark_bytes,
        config.server.tcp.output_maximum_capacity_bytes,
        config.server.tcp.socket_send_buffer_bytes,
        config.server.tcp.idle_timeout_ms);
    if (!tcp) {
        return config_failure<RuntimeOptions>(tcp.error());
    }
    auto http = http::HttpLimits::create(
        config.http.limits.max_request_line_bytes,
        config.http.limits.max_method_bytes,
        config.http.limits.max_target_bytes,
        config.http.limits.max_header_line_bytes,
        config.http.limits.max_header_bytes,
        config.http.limits.max_header_count, config.http.limits.max_body_bytes,
        config.http.limits.max_response_body_bytes,
        config.http.limits.max_routes,
        config.http.limits.max_requests_per_dispatch);
    if (!http) {
        return config_failure<RuntimeOptions>(http.error());
    }
    const task::ThreadPoolOptions pool{
        config.runtime.worker_threads,
        config.runtime.task_queue_capacity,
    };
    auto pool_valid = task::BoundedThreadPool::validate_options(pool);
    if (!pool_valid) {
        return config_failure<RuntimeOptions>(pool_valid.error());
    }
    auto tasks = task::TaskLimits::create(
        config.tasks.max_repository_tasks, config.tasks.max_operation_bytes,
        config.tasks.max_input_bytes, config.tasks.max_result_bytes,
        config.tasks.max_error_message_bytes, config.tasks.max_json_depth,
        config.tasks.max_json_elements, config.tasks.max_json_string_bytes);
    if (!tasks) {
        return config_failure<RuntimeOptions>(tasks.error());
    }
    auto plugins = plugin::PluginLimits::create(
        config.plugins.limits.max_plugins,
        config.plugins.limits.max_operation_bytes,
        config.plugins.limits.max_name_bytes,
        config.plugins.limits.max_version_bytes,
        config.plugins.limits.max_description_bytes,
        config.plugins.limits.max_error_message_bytes,
        config.plugins.limits.max_input_bytes,
        config.plugins.limits.max_output_bytes,
        config.plugins.limits.max_json_depth,
        config.plugins.limits.max_json_elements,
        config.plugins.limits.max_string_bytes,
        config.plugins.limits.max_capabilities,
        config.plugins.limits.max_capability_bytes);
    if (!plugins) {
        return config_failure<RuntimeOptions>(plugins.error());
    }
    auto api =
        api::TaskApiLimits::create(config.task_api.max_error_message_bytes,
                                   config.task_api.max_status_url_bytes);
    if (!api) {
        return config_failure<RuntimeOptions>(api.error());
    }

    std::size_t timer_layers = 0U;
    if (config.server.tcp.idle_timeout_ms.has_value()) {
        ++timer_layers;
    }
    if (config.http.header_timeout_ms.has_value() ||
        config.http.body_timeout_ms.has_value()) {
        ++timer_layers;
    }
    if (timer_layers != 0U) {
        const auto connections =
            static_cast<std::uint64_t>(config.server.tcp.max_connections);
        const auto required = connections * timer_layers;
        if (required > config.server.reactor.max_timers) {
            return Result<RuntimeOptions>::failure(
                make_error(ErrorCode::ConfigError,
                           "server.reactor.max_timers cannot cover "
                           "configured connection timeouts"));
        }
    }

    auto service = ServiceOptions::create(
        std::move(tcp).value(), std::move(http).value(), pool,
        std::move(tasks).value(), std::move(plugins).value(),
        std::move(api).value(), config.plugins.enable_echo,
        config.plugins.enable_mock_vision,
        as_timeout(config.http.header_timeout_ms),
        as_timeout(config.http.body_timeout_ms));
    if (!service) {
        return config_failure<RuntimeOptions>(service.error());
    }
    return Result<RuntimeOptions>::success(RuntimeOptions{
        config.server.reactor.max_events,
        config.server.reactor.pending_callback_capacity,
        timers.value(),
        std::move(endpoint).value(),
        std::move(service).value(),
    });
}

} // namespace iaisf::service
