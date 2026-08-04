#pragma once

#include <chrono>
#include <optional>

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
        bool enable_mock_vision = true,
        std::optional<std::chrono::milliseconds> http_header_timeout =
            std::nullopt,
        std::optional<std::chrono::milliseconds> http_body_timeout =
            std::nullopt);
    [[nodiscard]] static Result<ServiceOptions> defaults();

    [[nodiscard]] const net::tcp::TcpServerOptions& tcp_options() const noexcept;
    [[nodiscard]] const http::HttpLimits& http_limits() const noexcept;
    [[nodiscard]] task::ThreadPoolOptions pool_options() const noexcept;
    [[nodiscard]] const task::TaskLimits& task_limits() const noexcept;
    [[nodiscard]] const plugin::PluginLimits& plugin_limits() const noexcept;
    [[nodiscard]] const api::TaskApiLimits& api_limits() const noexcept;
    [[nodiscard]] bool enable_echo() const noexcept;
    [[nodiscard]] bool enable_mock_vision() const noexcept;
    [[nodiscard]] std::optional<std::chrono::milliseconds>
    http_header_timeout() const noexcept;
    [[nodiscard]] std::optional<std::chrono::milliseconds>
    http_body_timeout() const noexcept;

private:
    ServiceOptions(
        net::tcp::TcpServerOptions tcp_options,
        http::HttpLimits http_limits,
        task::ThreadPoolOptions pool_options,
        task::TaskLimits task_limits,
        plugin::PluginLimits plugin_limits,
        api::TaskApiLimits api_limits,
        bool enable_echo,
        bool enable_mock_vision,
        std::optional<std::chrono::milliseconds> http_header_timeout,
        std::optional<std::chrono::milliseconds> http_body_timeout) noexcept;

    net::tcp::TcpServerOptions tcp_options_;
    http::HttpLimits http_limits_;
    task::ThreadPoolOptions pool_options_;
    task::TaskLimits task_limits_;
    plugin::PluginLimits plugin_limits_;
    api::TaskApiLimits api_limits_;
    bool enable_echo_;
    bool enable_mock_vision_;
    std::optional<std::chrono::milliseconds> http_header_timeout_;
    std::optional<std::chrono::milliseconds> http_body_timeout_;
};

}  // namespace iaisf::service
