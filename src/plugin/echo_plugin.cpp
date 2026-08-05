#include "iaisf/plugin/echo_plugin.hpp"

#include <exception>
#include <new>

#include "iaisf/core/error.hpp"

namespace iaisf::plugin {

PluginMetadata EchoPlugin::metadata() const {
    return PluginMetadata{
        "echo",
        "Echo Plugin",
        "1.0.0",
        "Returns one supplied JSON payload without I/O.",
        false,
        {"deterministic", "stateless"},
    };
}

Result<void> EchoPlugin::validate_input(const nlohmann::json& input) const {
    if (!input.is_object()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "echo input must be an object"));
    }
    if (input.size() != 1U || !input.contains("payload")) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "echo input must contain only payload"));
    }
    return Result<void>::success();
}

Result<nlohmann::json> EchoPlugin::execute(
    const nlohmann::json& input) const {
    auto valid = validate_input(input);
    if (!valid) {
        return Result<nlohmann::json>::failure(std::move(valid).error());
    }

    try {
        return Result<nlohmann::json>::success(input.at("payload"));
    } catch (const std::bad_alloc&) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate echo result"));
    } catch (const std::exception&) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InternalError,
            "echo execution failed"));
    }
}

Result<void> EchoPlugin::initialize() {
    return Result<void>::success();
}

Result<void> EchoPlugin::shutdown() {
    return Result<void>::success();
}

}  // namespace iaisf::plugin
