#include "iaisf/service/service_options.hpp"

#include <limits>
#include <utility>

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
    const api::TaskApiLimits& api_options) {
    auto pool_valid = task::BoundedThreadPool::validate_options(pool);
    if (!pool_valid) {
        return pool_valid;
    }
    if (http.max_routes() < 4U) {
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
    const bool enable_mock_vision) {
    auto valid = validate_cross_limits(
        tcp_options,
        http_limits,
        pool_options,
        task_limits,
        plugin_limits,
        api_limits);
    if (!valid) {
        return Result<ServiceOptions>::failure(std::move(valid).error());
    }
    return Result<ServiceOptions>::success(ServiceOptions{
        std::move(tcp_options),
        std::move(http_limits),
        pool_options,
        std::move(task_limits),
        std::move(plugin_limits),
        std::move(api_limits),
        enable_echo,
        enable_mock_vision});
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
    return create(
        std::move(tcp).value(),
        http::HttpLimits::defaults(),
        task::ThreadPoolOptions{},
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
    const bool enable_mock_vision) noexcept
    : tcp_options_(std::move(tcp_options)),
      http_limits_(std::move(http_limits)),
      pool_options_(pool_options),
      task_limits_(std::move(task_limits)),
      plugin_limits_(std::move(plugin_limits)),
      api_limits_(std::move(api_limits)),
      enable_echo_(enable_echo),
      enable_mock_vision_(enable_mock_vision) {}

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

}  // namespace iaisf::service
