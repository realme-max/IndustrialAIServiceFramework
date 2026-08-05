#include "iaisf/service/industrial_ai_service.hpp"

#include <exception>
#include <new>
#include <utility>

#include "iaisf/core/error.hpp"
#include "iaisf/http/builtin_routes.hpp"
#include "iaisf/http/diagnostics_routes.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/mock_vision_plugin.hpp"

namespace iaisf::service {

struct IndustrialAiService::SignalShutdownState {
    IndustrialAiService* service{nullptr};
};

class SignalQueueRollback final {
public:
    explicit SignalQueueRollback(net::EventLoop& loop) noexcept
        : loop_(loop) {}

    SignalQueueRollback(const SignalQueueRollback&) = delete;
    SignalQueueRollback& operator=(const SignalQueueRollback&) = delete;

    ~SignalQueueRollback() noexcept {
        if (!active_) {
            return;
        }
        auto result = loop_.disable_shutdown_signals();
        if (!result) {
            std::terminate();
        }
    }

    void dismiss() noexcept { active_ = false; }

private:
    net::EventLoop& loop_;
    bool active_{true};
};

Result<IndustrialAiService::Ptr> IndustrialAiService::create(
    net::EventLoop& loop,
    ILogger& logger,
    const net::tcp::Ipv4Endpoint& bind_endpoint,
    ServiceOptions options,
    const ILogDiagnostics* const logger_diagnostics) {
    return create(
        loop,
        logger,
        bind_endpoint,
        std::move(options),
        {}, logger_diagnostics);
}

Result<IndustrialAiService::Ptr> IndustrialAiService::create(
    net::EventLoop& loop,
    ILogger& logger,
    const net::tcp::Ipv4Endpoint& bind_endpoint,
    ServiceOptions options,
    std::vector<std::shared_ptr<const plugin::IAlgorithmPlugin>>
        static_plugins,
    const ILogDiagnostics* const logger_diagnostics) {
    if (!loop.is_in_loop_thread()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidState,
            "service must be created in the EventLoop owner thread"));
    }

    try {
        auto signal_shutdown_state =
            std::make_shared<SignalShutdownState>();
        const std::weak_ptr<SignalShutdownState> weak_signal_state =
            signal_shutdown_state;
        auto signal_result = loop.enable_shutdown_signals(
            [weak_signal_state] {
                const auto state = weak_signal_state.lock();
                if (!state || state->service == nullptr) {
                    return;
                }
                auto result = state->service->stop();
                if (!result) {
                    state->service->safe_log_stop_failure(result.error());
                }
            });
        if (!signal_result) {
            return Result<Ptr>::failure(std::move(signal_result).error());
        }
        SignalQueueRollback signal_rollback{loop};

        auto health_checker = std::make_shared<health::HealthChecker>();

        auto plugin_manager =
            std::make_shared<plugin::PluginManager>(options.plugin_limits());
        if (options.enable_echo()) {
            auto registered = plugin_manager->register_plugin(
                std::make_shared<plugin::EchoPlugin>());
            if (!registered) {
                return Result<Ptr>::failure(std::move(registered).error());
            }
        }
        if (options.enable_mock_vision()) {
            auto registered = plugin_manager->register_plugin(
                std::make_shared<plugin::MockVisionPlugin>());
            if (!registered) {
                return Result<Ptr>::failure(std::move(registered).error());
            }
        }
        for (auto& static_plugin : static_plugins) {
            auto registered = plugin_manager->register_plugin(
                std::move(static_plugin));
            if (!registered) {
                return Result<Ptr>::failure(std::move(registered).error());
            }
        }
        auto frozen = plugin_manager->freeze();
        if (!frozen) {
            return Result<Ptr>::failure(std::move(frozen).error());
        }

        auto adapter_result =
            plugin::PluginTaskAdapter::create(plugin_manager);
        if (!adapter_result) {
            return Result<Ptr>::failure(std::move(adapter_result).error());
        }
        auto adapter = std::move(adapter_result).value();

        auto task_result = task::TaskManager::create(
            options.pool_options(),
            options.task_limits(),
            logger,
            adapter->make_validator(),
            adapter->make_handler(),
            loop.metrics_registry());
        if (!task_result) {
            return Result<Ptr>::failure(std::move(task_result).error());
        }
        auto task_manager = std::move(task_result).value();

        std::shared_ptr<diagnostics::RuntimeDiagnostics> diagnostics;
        if (options.diagnostics_enabled()) {
            auto* const metrics = loop.metrics_registry();
            if (metrics == nullptr) {
                return Result<Ptr>::failure(make_error(
                    ErrorCode::InvalidState,
                    "diagnostics endpoint requires an application metrics registry"));
            }
            auto diagnostics_result = diagnostics::RuntimeDiagnostics::create(
                health_checker, *task_manager, *metrics, logger_diagnostics);
            if (!diagnostics_result) {
                return Result<Ptr>::failure(std::move(diagnostics_result).error());
            }
            diagnostics = std::move(diagnostics_result).value();
        }

        auto api_result = api::TaskHttpApi::create(
            *task_manager,
            *plugin_manager,
            options.task_limits(),
            options.http_limits(),
            options.api_limits());
        if (!api_result) {
            return Result<Ptr>::failure(std::move(api_result).error());
        }
        auto task_api = std::move(api_result).value();

        http::HttpRouter router{options.http_limits()};
        auto builtins = http::register_builtin_routes(
            router,
            std::weak_ptr<const health::HealthChecker>{health_checker});
        if (!builtins) {
            return Result<Ptr>::failure(std::move(builtins).error());
        }
        auto task_routes = task_api->register_routes(router);
        if (!task_routes) {
            return Result<Ptr>::failure(std::move(task_routes).error());
        }
        if (options.metrics_enabled()) {
            auto* const metrics = loop.metrics_registry();
            if (metrics == nullptr) {
                return Result<Ptr>::failure(make_error(
                    ErrorCode::InvalidState,
                    "metrics endpoint requires an application metrics registry"));
            }
            auto metrics_route = http::register_metrics_route(
                router, *metrics, options.metrics_endpoint());
            if (!metrics_route) {
                return Result<Ptr>::failure(std::move(metrics_route).error());
            }
        }
        if (options.diagnostics_enabled()) {
            auto diagnostics_route = http::register_diagnostics_route(
                router, std::weak_ptr<const diagnostics::RuntimeDiagnostics>{diagnostics},
                options.diagnostics_endpoint(), options.http_limits().max_response_body_bytes());
            if (!diagnostics_route) {
                return Result<Ptr>::failure(std::move(diagnostics_route).error());
            }
        }
        auto router_frozen = router.freeze();
        if (!router_frozen) {
            return Result<Ptr>::failure(std::move(router_frozen).error());
        }

        auto http_result = http::HttpServer::create(
            loop,
            logger,
            bind_endpoint,
            options.tcp_options(),
            std::move(router),
            options.http_limits(),
            options.http_header_timeout(),
            options.http_body_timeout(),
            loop.metrics_registry());
        if (!http_result) {
            return Result<Ptr>::failure(std::move(http_result).error());
        }

        auto service = Ptr{new IndustrialAiService{
            ConstructionKey{},
            loop,
            logger,
            std::move(plugin_manager),
            std::move(adapter),
            std::move(task_manager),
            std::move(task_api),
            std::move(http_result).value(),
            std::move(health_checker),
            std::move(diagnostics),
            std::move(signal_shutdown_state)}};
        service->signal_shutdown_state_->service = service.get();
        signal_rollback.dismiss();
        return Result<Ptr>::success(std::move(service));
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate industrial AI service"));
    } catch (const std::exception&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InternalError,
            "unable to construct industrial AI service"));
    }
}

IndustrialAiService::IndustrialAiService(
    ConstructionKey,
    net::EventLoop& loop,
    ILogger& logger,
    std::shared_ptr<plugin::PluginManager> plugin_manager,
    std::shared_ptr<plugin::PluginTaskAdapter> plugin_adapter,
    std::unique_ptr<task::TaskManager> task_manager,
    api::TaskHttpApi::Ptr task_api,
    http::HttpServer::Ptr http_server,
    std::shared_ptr<health::HealthChecker> health_checker,
    std::shared_ptr<diagnostics::RuntimeDiagnostics> diagnostics,
    std::shared_ptr<SignalShutdownState> signal_shutdown_state) noexcept
    : loop_(loop),
      logger_(logger),
      plugin_manager_(std::move(plugin_manager)),
      plugin_adapter_(std::move(plugin_adapter)),
      task_manager_(std::move(task_manager)),
      task_api_(std::move(task_api)),
      health_checker_(std::move(health_checker)),
      diagnostics_(std::move(diagnostics)),
      http_server_(std::move(http_server)),
      signal_shutdown_state_(std::move(signal_shutdown_state)),
      stop_continuation_(this, &IndustrialAiService::run_stop_continuation) {}

IndustrialAiService::~IndustrialAiService() noexcept {
    if (signal_shutdown_state_) {
        signal_shutdown_state_->service = nullptr;
    }
    if (state_ == State::Created) {
        const auto result = stop();
        if (!result) {
            std::terminate();
        }
    }
    if (!stopped()) {
        std::terminate();
    }
    (void)health_checker_->transition_to(health::HealthPhase::Stopped);
}

Result<void> IndustrialAiService::start() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "service start must run in the EventLoop owner thread"));
    }
    if (state_ != State::Created) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "service can only be started once"));
    }
    auto result = http_server_->start();
    if (!result) {
        (void)health_checker_->transition_to(health::HealthPhase::Stopping);
        state_ = State::StoppingHttp;
        task_api_->stop_admission();
        auto rolled_back = advance_stop();
        if (!rolled_back) {
            return rolled_back;
        }
        if (state_ == State::Stopped) {
            (void)health_checker_->transition_to(health::HealthPhase::Stopped);
        }
        return result;
    }
    const auto health_transition = health_checker_->transition_to(
        health::HealthPhase::Running);
    if (health_transition == health::HealthTransitionOutcome::InvalidTransition) {
        (void)health_checker_->transition_to(health::HealthPhase::Stopping);
        state_ = State::StoppingHttp;
        task_api_->stop_admission();
        (void)advance_stop();
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "health checker rejected service startup"));
    }
    state_ = State::Running;
    return Result<void>::success();
}

Result<void> IndustrialAiService::stop() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "service stop must run in the EventLoop owner thread"));
    }
    if (state_ == State::Stopped) {
        return Result<void>::success();
    }
    if (state_ == State::Created || state_ == State::Running) {
        (void)health_checker_->transition_to(health::HealthPhase::Stopping);
        state_ = State::StoppingHttp;
        task_api_->stop_admission();
    }
    return advance_stop();
}

Result<void> IndustrialAiService::advance_stop() {
    if (state_ == State::StoppingHttp) {
        auto http_stop = http_server_->stop();
        if (!http_stop) {
            return http_stop;
        }
        if (!http_server_->stopped()) {
            return schedule_stop_continuation();
        }
        if (http_server_->session_count() != 0U ||
            http_server_->connection_count() != 0U) {
            return Result<void>::failure(make_error(
                ErrorCode::InternalError,
                "HTTP server reported stopped with live transport state"));
        }
        state_ = State::StoppingTasks;
    }

    if (state_ == State::StoppingTasks) {
        // This join is intentionally blocking. It is reached only after all
        // HTTP Channels and Sessions are gone, so EventLoop cleanup no longer
        // depends on the owner thread while accepted task work drains.
        auto tasks_stop = task_manager_->shutdown();
        if (!tasks_stop) {
            return tasks_stop;
        }
        if (!task_manager_->stopped()) {
            return Result<void>::failure(make_error(
                ErrorCode::InternalError,
                "task manager shutdown returned before workers stopped"));
        }
        (void)health_checker_->transition_to(health::HealthPhase::Stopped);
        state_ = State::Stopped;
    }
    return Result<void>::success();
}

Result<void> IndustrialAiService::schedule_stop_continuation() {
    if (stop_continuation_.pending()) {
        return Result<void>::success();
    }
    auto scheduled = loop_.defer_cleanup(stop_continuation_);
    if (!scheduled) {
        return Result<void>::failure(std::move(scheduled).error());
    }
    return Result<void>::success();
}

void IndustrialAiService::run_stop_continuation(
    void* const context) noexcept {
    auto* const service = static_cast<IndustrialAiService*>(context);
    try {
        auto stopped = service->advance_stop();
        if (!stopped) {
            service->safe_log_stop_failure(stopped.error());
        }
    } catch (...) {
        try {
            service->logger_.log(
                LogLevel::Error,
                "IndustrialAiService",
                "service stop continuation failed");
        } catch (...) {
        }
    }
}

void IndustrialAiService::safe_log_stop_failure(
    const Error& error) noexcept {
    try {
        logger_.log(LogLevel::Error, "IndustrialAiService", error.message);
    } catch (...) {
    }
}

IndustrialAiService::State IndustrialAiService::state() const noexcept {
    return state_;
}

bool IndustrialAiService::stopped() const noexcept {
    return state_ == State::Stopped &&
           http_server_->stopped() &&
           http_server_->session_count() == 0U &&
           http_server_->connection_count() == 0U &&
           task_manager_->stopped();
}

const net::tcp::Ipv4Endpoint&
IndustrialAiService::local_endpoint() const noexcept {
    return http_server_->local_endpoint();
}

const task::TaskManager&
IndustrialAiService::task_manager() const noexcept {
    return *task_manager_;
}

std::size_t IndustrialAiService::session_count() const noexcept {
    return http_server_->session_count();
}

std::size_t IndustrialAiService::connection_count() const noexcept {
    return http_server_->connection_count();
}

health::HealthStatus IndustrialAiService::health_status() const noexcept {
    return health_checker_->snapshot();
}

}  // namespace iaisf::service
