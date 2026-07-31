#include "iaisf/plugin/plugin_manager.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "iaisf/core/error.hpp"
#include "iaisf/core/json_value_limits.hpp"

namespace iaisf::plugin {
namespace {

std::string bounded_message(
    const std::string_view message,
    const std::size_t maximum) {
    return std::string(message.substr(0, maximum));
}

}  // namespace

PluginManager::PluginManager(PluginLimits limits)
    : limits_(std::move(limits)) {}

Result<void> PluginManager::register_plugin(
    std::shared_ptr<const IAlgorithmPlugin> plugin) {
    if (!plugin) {
        return Result<void>::failure(bounded_error(
            ErrorCode::InvalidArgument,
            "plugin must not be null"));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frozen_) {
            return Result<void>::failure(bounded_error(
                ErrorCode::InvalidState,
                "plugin registry is frozen"));
        }
    }

    PluginMetadata metadata;
    try {
        metadata = plugin->metadata();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin metadata"));
    } catch (const std::exception&) {
        return Result<void>::failure(
            fixed_internal_error("plugin metadata failed"));
    } catch (...) {
        return Result<void>::failure(
            fixed_internal_error("plugin metadata failed"));
    }

    try {
        auto valid = validate_metadata(metadata, limits_);
        if (!valid) {
            return Result<void>::failure(bounded_error(
                valid.error().code,
                valid.error().message));
        }
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "unable to validate plugin metadata"));
    } catch (const std::length_error&) {
        return Result<void>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "plugin metadata exceeds the platform size limit"));
    } catch (const std::exception&) {
        return Result<void>::failure(
            fixed_internal_error("plugin metadata validation failed"));
    } catch (...) {
        return Result<void>::failure(
            fixed_internal_error("plugin metadata validation failed"));
    }

    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frozen_) {
            return Result<void>::failure(bounded_error(
                ErrorCode::InvalidState,
                "plugin registry is frozen"));
        }
        if (registry_.find(metadata.operation) != registry_.end()) {
            return Result<void>::failure(bounded_error(
                ErrorCode::InvalidState,
                "plugin operation is already registered"));
        }
        if (registry_.size() >= limits_.max_plugins()) {
            return Result<void>::failure(bounded_error(
                ErrorCode::ResourceExhausted,
                "plugin registry capacity is exhausted"));
        }

        std::string operation = metadata.operation;
        const auto inserted = registry_.emplace(
            std::move(operation),
            Entry{std::move(metadata), std::move(plugin)});
        if (!inserted.second) {
            return Result<void>::failure(bounded_error(
                ErrorCode::InvalidState,
                "plugin operation is already registered"));
        }
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin registry entry"));
    } catch (const std::length_error&) {
        return Result<void>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "plugin registry entry exceeds the platform size limit"));
    } catch (const std::exception&) {
        return Result<void>::failure(
            fixed_internal_error("plugin registration failed"));
    } catch (...) {
        return Result<void>::failure(
            fixed_internal_error("plugin registration failed"));
    }
}

Result<void> PluginManager::freeze() {
    std::lock_guard<std::mutex> lock(mutex_);
    frozen_ = true;
    return Result<void>::success();
}

bool PluginManager::frozen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frozen_;
}

std::size_t PluginManager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_.size();
}

Result<PluginMetadata> PluginManager::find_metadata(
    const std::string_view operation) const {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!frozen_) {
            return Result<PluginMetadata>::failure(bounded_error(
                ErrorCode::InvalidState,
                "plugin registry is not frozen"));
        }
        auto valid = validate_plugin_operation(operation, limits_);
        if (!valid) {
            return Result<PluginMetadata>::failure(
                bounded_error(valid.error().code, valid.error().message));
        }
        const auto found = registry_.find(std::string(operation));
        if (found == registry_.end()) {
            return Result<PluginMetadata>::failure(bounded_error(
                ErrorCode::NotFound,
                "plugin operation was not found"));
        }
        return Result<PluginMetadata>::success(found->second.metadata);
    } catch (const std::bad_alloc&) {
        return Result<PluginMetadata>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin metadata result"));
    } catch (const std::length_error&) {
        return Result<PluginMetadata>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "plugin operation exceeds the platform size limit"));
    }
}

Result<std::vector<PluginMetadata>> PluginManager::list_metadata() const {
    try {
        std::vector<PluginMetadata> metadata;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!frozen_) {
                return Result<std::vector<PluginMetadata>>::failure(bounded_error(
                    ErrorCode::InvalidState,
                    "plugin registry is not frozen"));
            }
            metadata.reserve(registry_.size());
            for (const auto& item : registry_) {
                metadata.push_back(item.second.metadata);
            }
        }
        std::sort(
            metadata.begin(),
            metadata.end(),
            [](const PluginMetadata& lhs, const PluginMetadata& rhs) {
                return lhs.operation < rhs.operation;
            });
        return Result<std::vector<PluginMetadata>>::success(
            std::move(metadata));
    } catch (const std::bad_alloc&) {
        return Result<std::vector<PluginMetadata>>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate plugin metadata list"));
    } catch (const std::length_error&) {
        return Result<std::vector<PluginMetadata>>::failure(bounded_error(
            ErrorCode::ResourceExhausted,
            "plugin metadata list exceeds the platform size limit"));
    }
}

Result<std::shared_ptr<const IAlgorithmPlugin>> PluginManager::find_plugin(
    const std::string_view operation) const {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!frozen_) {
            return Result<std::shared_ptr<const IAlgorithmPlugin>>::failure(
                bounded_error(
                    ErrorCode::InvalidState,
                    "plugin registry is not frozen"));
        }
        auto valid = validate_plugin_operation(operation, limits_);
        if (!valid) {
            return Result<std::shared_ptr<const IAlgorithmPlugin>>::failure(
                bounded_error(valid.error().code, valid.error().message));
        }
        const auto found = registry_.find(std::string(operation));
        if (found == registry_.end()) {
            return Result<std::shared_ptr<const IAlgorithmPlugin>>::failure(
                bounded_error(
                    ErrorCode::NotFound,
                    "plugin operation was not found"));
        }
        return Result<std::shared_ptr<const IAlgorithmPlugin>>::success(
            found->second.plugin);
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<const IAlgorithmPlugin>>::failure(
            bounded_error(
                ErrorCode::ResourceExhausted,
                "unable to allocate plugin lookup key"));
    } catch (const std::length_error&) {
        return Result<std::shared_ptr<const IAlgorithmPlugin>>::failure(
            bounded_error(
                ErrorCode::ResourceExhausted,
                "plugin operation exceeds the platform size limit"));
    }
}

Result<void> PluginManager::validate(
    const std::string_view operation,
    const nlohmann::json& input) const {
    auto plugin = find_plugin(operation);
    if (!plugin) {
        return Result<void>::failure(std::move(plugin).error());
    }

    try {
        auto capacity = validate_input_capacity(input);
        if (!capacity) {
            return capacity;
        }
        return invoke_plugin_validation(plugin.value(), input);
    } catch (const std::exception&) {
        return Result<void>::failure(
            fixed_internal_error("plugin input validation failed"));
    } catch (...) {
        return Result<void>::failure(
            fixed_internal_error("plugin input validation failed"));
    }
}

Result<nlohmann::json> PluginManager::execute(
    const std::string_view operation,
    const nlohmann::json& input) const {
    auto valid = validate(operation, input);
    if (!valid) {
        return Result<nlohmann::json>::failure(std::move(valid).error());
    }
    return execute_validated(operation, input);
}

Result<nlohmann::json> PluginManager::execute_validated(
    const std::string_view operation,
    const nlohmann::json& input) const {
    auto plugin = find_plugin(operation);
    if (!plugin) {
        return Result<nlohmann::json>::failure(std::move(plugin).error());
    }

    try {
        auto result = plugin.value()->execute(input);
        if (!result) {
            return Result<nlohmann::json>::failure(
                normalized_plugin_error(result.error(), false));
        }
        auto output_valid = validate_output_capacity(result.value());
        if (!output_valid) {
            return Result<nlohmann::json>::failure(
                std::move(output_valid).error());
        }
        return result;
    } catch (const std::exception&) {
        return Result<nlohmann::json>::failure(
            fixed_internal_error("plugin execution failed"));
    } catch (...) {
        return Result<nlohmann::json>::failure(
            fixed_internal_error("plugin execution failed"));
    }
}

Result<void> PluginManager::validate_plugin_contract(
    const std::string_view operation,
    const nlohmann::json& input) const {
    auto plugin = find_plugin(operation);
    if (!plugin) {
        return Result<void>::failure(std::move(plugin).error());
    }
    return invoke_plugin_validation(plugin.value(), input);
}

Result<void> PluginManager::invoke_plugin_validation(
    const std::shared_ptr<const IAlgorithmPlugin>& plugin,
    const nlohmann::json& input) const {
    try {
        auto result = plugin->validate_input(input);
        if (!result) {
            return Result<void>::failure(
                normalized_plugin_error(result.error(), true));
        }
        return Result<void>::success();
    } catch (const std::exception&) {
        return Result<void>::failure(
            fixed_internal_error("plugin validation failed"));
    } catch (...) {
        return Result<void>::failure(
            fixed_internal_error("plugin validation failed"));
    }
}

Result<void> PluginManager::validate_input_capacity(
    const nlohmann::json& input) const {
    const auto valid = validate_json_value(
        input,
        JsonValueLimits{
            limits_.max_input_bytes(),
            limits_.max_json_depth(),
            limits_.max_json_elements(),
            limits_.max_string_bytes(),
        },
        "plugin input",
        ErrorCode::InvalidArgument);
    if (!valid) {
        return Result<void>::failure(
            bounded_error(valid.error().code, valid.error().message));
    }
    return Result<void>::success();
}

Result<void> PluginManager::validate_output_capacity(
    const nlohmann::json& output) const {
    const auto valid = validate_json_value(
        output,
        JsonValueLimits{
            limits_.max_output_bytes(),
            limits_.max_json_depth(),
            limits_.max_json_elements(),
            limits_.max_string_bytes(),
        },
        "plugin output",
        ErrorCode::InternalError);
    if (!valid) {
        return Result<void>::failure(
            bounded_error(valid.error().code, valid.error().message));
    }
    return Result<void>::success();
}

const PluginLimits& PluginManager::limits() const noexcept {
    return limits_;
}

Error PluginManager::normalized_plugin_error(
    const Error& error,
    const bool validation) const {
    const bool safe_code =
        validation &&
        (error.code == ErrorCode::InvalidArgument ||
         error.code == ErrorCode::ResourceExhausted);
    if (!safe_code) {
        return fixed_internal_error(
            validation ? "plugin validation failed" : "plugin execution failed");
    }

    const auto maximum = limits_.max_error_message_bytes();
    if (!error.message.empty() && error.message.size() <= maximum) {
        return make_error(error.code, error.message);
    }
    return make_error(
        error.code,
        bounded_message(
            validation ? "plugin input invalid" : "plugin resource exhausted",
            maximum));
}

Error PluginManager::fixed_internal_error(
    const std::string_view message) const {
    return make_error(
        ErrorCode::InternalError,
        bounded_message(message, limits_.max_error_message_bytes()));
}

Error PluginManager::bounded_error(
    const ErrorCode code,
    const std::string_view message) const {
    return make_error(
        code,
        bounded_message(message, limits_.max_error_message_bytes()));
}

}  // namespace iaisf::plugin
