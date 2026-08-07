#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "iaisf/api/task_api_limits.hpp"
#include "iaisf/application/application_adapters.hpp"
#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/net/tcp/tcp_server.hpp"
#include "iaisf/plugin/plugin_limits.hpp"
#include "iaisf/task/task_limits.hpp"
#include "iaisf/task/thread_pool.hpp"

namespace iaisf::service {

struct DynamicPluginModuleOptions {
    std::string id;
    std::filesystem::path library;
    std::string config_json{"{}"};
};

struct DynamicPluginOptions {
    bool enabled{false};
    std::filesystem::path root{"plugins"};
    std::size_t max_modules{16U};
    std::vector<DynamicPluginModuleOptions> modules;
};

struct ApplicationRuntimeOptions {
    bool enabled{false};
    std::filesystem::path artifact_root;
    std::filesystem::path scratch_root;
    std::filesystem::path output_root;
    std::size_t repository_capacity{1024U};
    std::size_t queue_capacity{128U};
    application::Ptv2AdapterOptions ptv2;
    application::WeldAgentAdapterOptions weld_agent;
};

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
            std::nullopt,
        bool metrics_enabled = false,
        std::string metrics_endpoint = "/metrics",
        bool diagnostics_enabled = false,
        std::string diagnostics_endpoint = "/debug/status",
        DynamicPluginOptions dynamic_plugins = {},
        ApplicationRuntimeOptions applications = {});
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
    [[nodiscard]] bool metrics_enabled() const noexcept;
    [[nodiscard]] const std::string& metrics_endpoint() const noexcept;
    [[nodiscard]] bool diagnostics_enabled() const noexcept;
    [[nodiscard]] const std::string& diagnostics_endpoint() const noexcept;
    [[nodiscard]] const DynamicPluginOptions& dynamic_plugins() const noexcept;
    [[nodiscard]] const ApplicationRuntimeOptions& applications() const noexcept;

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
        std::optional<std::chrono::milliseconds> http_body_timeout,
        bool metrics_enabled,
        std::string metrics_endpoint,
        bool diagnostics_enabled,
        std::string diagnostics_endpoint,
        DynamicPluginOptions dynamic_plugins,
        ApplicationRuntimeOptions applications) noexcept;

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
    bool metrics_enabled_{false};
    std::string metrics_endpoint_;
    bool diagnostics_enabled_{false};
    std::string diagnostics_endpoint_;
    DynamicPluginOptions dynamic_plugins_;
    ApplicationRuntimeOptions applications_;
};

}  // namespace iaisf::service
