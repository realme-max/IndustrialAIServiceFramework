#pragma once

#include <memory>
#include <vector>

#include "iaisf/api/task_http_api.hpp"
#include "iaisf/core/result.hpp"
#include "iaisf/diagnostics/runtime_diagnostics.hpp"
#include "iaisf/http/http_server.hpp"
#include "iaisf/health/health_checker.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/logging/log_diagnostics.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/plugin/plugin_runtime.hpp"
#include "iaisf/plugin/plugin_task_adapter.hpp"
#include "iaisf/service/service_options.hpp"
#include "iaisf/task/task_manager.hpp"

namespace iaisf::service {

/**
 * Linux owner-thread composition root for HTTP, tasks, and the in-process
 * PluginRuntime.
 *
 * EventLoop and ILogger are borrowed. start()/stop() are owner-thread-only;
 * creation installs the process-wide SIGINT/SIGTERM shutdown callback on the
 * EventLoop. stop first closes HTTP admission, then drains and joins
 * TaskManager. It does not stop the externally owned EventLoop; the signal
 * callback invokes this stop flow before EventLoop::stop(). PluginRuntime is
 * kept Frozen while TaskManager drains and is shut down afterwards.
 */
class IndustrialAiService final {
    struct ConstructionKey {
        explicit ConstructionKey() = default;
    };
    struct SignalShutdownState;

public:
    using Ptr = std::unique_ptr<IndustrialAiService>;

    enum class State {
        Created,
        Running,
        StoppingHttp,
        StoppingTasks,
        Stopped,
    };

    [[nodiscard]] static Result<Ptr> create(
        net::EventLoop& loop,
        ILogger& logger,
        const net::tcp::Ipv4Endpoint& bind_endpoint,
        ServiceOptions options,
        const ILogDiagnostics* logger_diagnostics = nullptr);
    [[nodiscard]] static Result<Ptr> create(
        net::EventLoop& loop,
        ILogger& logger,
        const net::tcp::Ipv4Endpoint& bind_endpoint,
        ServiceOptions options,
        std::vector<std::shared_ptr<const plugin::IAlgorithmPlugin>>
            static_plugins,
        const ILogDiagnostics* logger_diagnostics = nullptr);

    IndustrialAiService(const IndustrialAiService&) = delete;
    IndustrialAiService& operator=(const IndustrialAiService&) = delete;
    IndustrialAiService(IndustrialAiService&&) = delete;
    IndustrialAiService& operator=(IndustrialAiService&&) = delete;
    ~IndustrialAiService() noexcept;

    IndustrialAiService(
        ConstructionKey,
        net::EventLoop& loop,
        ILogger& logger,
        std::shared_ptr<plugin::PluginRuntime> plugin_runtime,
        std::shared_ptr<plugin::PluginTaskAdapter> plugin_adapter,
        std::unique_ptr<task::TaskManager> task_manager,
        api::TaskHttpApi::Ptr task_api,
        http::HttpServer::Ptr http_server,
        std::shared_ptr<health::HealthChecker> health_checker,
        std::shared_ptr<diagnostics::RuntimeDiagnostics> diagnostics,
        std::shared_ptr<SignalShutdownState> signal_shutdown_state) noexcept;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> stop();

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] const net::tcp::Ipv4Endpoint& local_endpoint() const noexcept;
    [[nodiscard]] const task::TaskManager& task_manager() const noexcept;
    [[nodiscard]] std::size_t session_count() const noexcept;
    [[nodiscard]] std::size_t connection_count() const noexcept;
    /** Returns the lock-free health snapshot for this service lifecycle. */
    [[nodiscard]] health::HealthStatus health_status() const noexcept;

private:
    [[nodiscard]] Result<void> advance_stop();
    [[nodiscard]] Result<void> schedule_stop_continuation();
    static void run_stop_continuation(void* context) noexcept;
    void safe_log_stop_failure(const Error& error) noexcept;

    net::EventLoop& loop_;
    ILogger& logger_;

    // Reverse destruction order is part of the lifetime contract.
    std::shared_ptr<plugin::PluginRuntime> plugin_runtime_;
    std::shared_ptr<plugin::PluginTaskAdapter> plugin_adapter_;
    std::unique_ptr<task::TaskManager> task_manager_;
    api::TaskHttpApi::Ptr task_api_;
    std::shared_ptr<health::HealthChecker> health_checker_;
    std::shared_ptr<diagnostics::RuntimeDiagnostics> diagnostics_;
    http::HttpServer::Ptr http_server_;
    std::shared_ptr<SignalShutdownState> signal_shutdown_state_;
    net::EventLoop::DeferredCleanup stop_continuation_;
    State state_{State::Created};
};

}  // namespace iaisf::service
