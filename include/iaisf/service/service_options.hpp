#pragma once

#include "iaisf/api/task_api_limits.hpp"
#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/net/tcp/tcp_server.hpp"
#include "iaisf/plugin/plugin_limits.hpp"
#include "iaisf/task/task_limits.hpp"
#include "iaisf/task/thread_pool.hpp"

namespace iaisf::service {

/** Validated aggregate configuration for the Phase 7 composition root. */
class ServiceOptions final {
public:
    [[nodiscard]] static Result<ServiceOptions> create(
        net::tcp::TcpServerOptions tcp_options,
        http::HttpLimits http_limits,
        task::ThreadPoolOptions pool_options,
        task::TaskLimits task_limits,
        plugin::PluginLimits plugin_limits,
        api::TaskApiLimits api_limits,
        bool enable_echo = true,
        bool enable_mock_vision = true);
    [[nodiscard]] static Result<ServiceOptions> defaults();

    [[nodiscard]] const net::tcp::TcpServerOptions& tcp_options() const noexcept;
    [[nodiscard]] const http::HttpLimits& http_limits() const noexcept;
    [[nodiscard]] task::ThreadPoolOptions pool_options() const noexcept;
    [[nodiscard]] const task::TaskLimits& task_limits() const noexcept;
    [[nodiscard]] const plugin::PluginLimits& plugin_limits() const noexcept;
    [[nodiscard]] const api::TaskApiLimits& api_limits() const noexcept;
    [[nodiscard]] bool enable_echo() const noexcept;
    [[nodiscard]] bool enable_mock_vision() const noexcept;

private:
    ServiceOptions(
        net::tcp::TcpServerOptions tcp_options,
        http::HttpLimits http_limits,
        task::ThreadPoolOptions pool_options,
        task::TaskLimits task_limits,
        plugin::PluginLimits plugin_limits,
        api::TaskApiLimits api_limits,
        bool enable_echo,
        bool enable_mock_vision) noexcept;

    net::tcp::TcpServerOptions tcp_options_;
    http::HttpLimits http_limits_;
    task::ThreadPoolOptions pool_options_;
    task::TaskLimits task_limits_;
    plugin::PluginLimits plugin_limits_;
    api::TaskApiLimits api_limits_;
    bool enable_echo_;
    bool enable_mock_vision_;
};

}  // namespace iaisf::service
