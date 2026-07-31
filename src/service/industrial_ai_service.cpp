#include "iaisf/service/industrial_ai_service.hpp"

#include <exception>
#include <new>
#include <utility>

#include "iaisf/core/error.hpp"
#include "iaisf/http/builtin_routes.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/mock_vision_plugin.hpp"

namespace iaisf::service {

Result<IndustrialAiService::Ptr> IndustrialAiService::create(
    net::EventLoop& loop,
    ILogger& logger,
    const net::tcp::Ipv4Endpoint& bind_endpoint,
    ServiceOptions options) {
    return create(
        loop,
        logger,
        bind_endpoint,
        std::move(options),
        {});
}

Result<IndustrialAiService::Ptr> IndustrialAiService::create(
    net::EventLoop& loop,
    ILogger& logger,
    const net::tcp::Ipv4Endpoint& bind_endpoint,
    ServiceOptions options,
    std::vector<std::shared_ptr<const plugin::IAlgorithmPlugin>>
        static_plugins) {
    if (!loop.is_in_loop_thread()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidState,
            "service must be created in the EventLoop owner thread"));
    }

    try {
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
            adapter->make_handler());
        if (!task_result) {
            return Result<Ptr>::failure(std::move(task_result).error());
        }
        auto task_manager = std::move(task_result).value();

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
        auto builtins = http::register_builtin_routes(router);
        if (!builtins) {
            return Result<Ptr>::failure(std::move(builtins).error());
        }
        auto task_routes = task_api->register_routes(router);
        if (!task_routes) {
            return Result<Ptr>::failure(std::move(task_routes).error());
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
            options.http_limits());
        if (!http_result) {
            return Result<Ptr>::failure(std::move(http_result).error());
        }

        return Result<Ptr>::success(Ptr{new IndustrialAiService{
            ConstructionKey{},
            loop,
            logger,
            std::move(plugin_manager),
            std::move(adapter),
            std::move(task_manager),
            std::move(task_api),
            std::move(http_result).value()}});
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
    http::HttpServer::Ptr http_server) noexcept
    : loop_(loop),
      logger_(logger),
      plugin_manager_(std::move(plugin_manager)),
      plugin_adapter_(std::move(plugin_adapter)),
      task_manager_(std::move(task_manager)),
      task_api_(std::move(task_api)),
      http_server_(std::move(http_server)),
      stop_continuation_(this, &IndustrialAiService::run_stop_continuation) {}

IndustrialAiService::~IndustrialAiService() noexcept {
    if (state_ == State::Created) {
        const auto result = stop();
        if (!result) {
            std::terminate();
        }
    }
    if (!stopped()) {
        std::terminate();
    }
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
        state_ = State::StoppingHttp;
        task_api_->stop_admission();
        auto rolled_back = advance_stop();
        if (!rolled_back) {
            return rolled_back;
        }
        return result;
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

}  // namespace iaisf::service
