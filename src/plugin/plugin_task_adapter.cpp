#include "iaisf/plugin/plugin_task_adapter.hpp"

#include <exception>
#include <new>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::plugin {

Result<std::shared_ptr<PluginTaskAdapter>> PluginTaskAdapter::create(
    std::shared_ptr<PluginManager> manager) {
    if (!manager) {
        return Result<std::shared_ptr<PluginTaskAdapter>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin manager must not be null"));
    }
    if (!manager->frozen()) {
        return Result<std::shared_ptr<PluginTaskAdapter>>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin manager must be frozen"));
    }

    try {
        return Result<std::shared_ptr<PluginTaskAdapter>>::success(
            std::make_shared<PluginTaskAdapter>(
                ConstructionKey{},
                std::move(manager)));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<PluginTaskAdapter>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin task adapter"));
    } catch (const std::exception&) {
        return Result<std::shared_ptr<PluginTaskAdapter>>::failure(make_error(
            ErrorCode::InternalError,
            "unable to construct plugin task adapter"));
    }
}

Result<std::shared_ptr<PluginTaskAdapter>> PluginTaskAdapter::create(
    std::shared_ptr<PluginRuntime> runtime) {
    if (!runtime) {
        return Result<std::shared_ptr<PluginTaskAdapter>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin runtime must not be null"));
    }
    if (!runtime->frozen()) {
        return Result<std::shared_ptr<PluginTaskAdapter>>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin runtime must be frozen"));
    }

    try {
        return Result<std::shared_ptr<PluginTaskAdapter>>::success(
            std::make_shared<PluginTaskAdapter>(
                ConstructionKey{}, std::move(runtime)));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<PluginTaskAdapter>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin task adapter"));
    } catch (const std::exception&) {
        return Result<std::shared_ptr<PluginTaskAdapter>>::failure(make_error(
            ErrorCode::InternalError,
            "unable to construct plugin task adapter"));
    }
}

PluginTaskAdapter::PluginTaskAdapter(
    ConstructionKey,
    std::shared_ptr<PluginManager> manager)
    : manager_(std::move(manager)) {}

PluginTaskAdapter::PluginTaskAdapter(
    ConstructionKey,
    std::shared_ptr<PluginRuntime> runtime)
    : runtime_(std::move(runtime)) {}

Result<void> PluginTaskAdapter::validate_task(
    const task::TaskRequest& request) const {
    if (runtime_) {
        return runtime_->validate(request.operation, request.input);
    }
    return manager_->validate(request.operation, request.input);
}

Result<nlohmann::json> PluginTaskAdapter::execute_task(
    const task::TaskRequest& request) const {
    if (runtime_) {
        auto valid = runtime_->validate_for_execution(
            request.operation,
            request.input);
        if (!valid) {
            return Result<nlohmann::json>::failure(make_error(
                ErrorCode::InternalError,
                "plugin validation changed before execution"));
        }
        return runtime_->execute_validated(
            request.operation,
            request.input);
    }
    auto valid = manager_->validate_plugin_contract(
        request.operation,
        request.input);
    if (!valid) {
        return Result<nlohmann::json>::failure(
            manager_->fixed_internal_error(
                "plugin validation changed before execution"));
    }
    return manager_->execute_validated(request.operation, request.input);
}

task::TaskValidator PluginTaskAdapter::make_validator() const {
    if (runtime_) {
        const auto runtime = runtime_;
        return [runtime](const task::TaskRequest& request) {
            return runtime->validate(request.operation, request.input);
        };
    }
    const auto manager = manager_;
    return [manager](const task::TaskRequest& request) {
        return manager->validate(request.operation, request.input);
    };
}

task::TaskHandler PluginTaskAdapter::make_handler() const {
    if (runtime_) {
        const auto runtime = runtime_;
        return [runtime](const task::TaskRequest& request) {
            auto valid = runtime->validate_for_execution(
                request.operation,
                request.input);
            if (!valid) {
                return Result<nlohmann::json>::failure(make_error(
                    ErrorCode::InternalError,
                    "plugin validation changed before execution"));
            }
            return runtime->execute_validated(
                request.operation,
                request.input);
        };
    }
    const auto manager = manager_;
    return [manager](const task::TaskRequest& request) {
        auto valid = manager->validate_plugin_contract(
            request.operation,
            request.input);
        if (!valid) {
            return Result<nlohmann::json>::failure(
                manager->fixed_internal_error(
                    "plugin validation changed before execution"));
        }
        return manager->execute_validated(request.operation, request.input);
    };
}

}  // namespace iaisf::plugin
