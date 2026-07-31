#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/plugin/plugin_limits.hpp"

namespace iaisf::plugin {

struct PluginMetadata {
    std::string operation;
    std::string name;
    std::string version;
    std::string description;
    bool mock{false};
    std::vector<std::string> capabilities;
};

[[nodiscard]] Result<void> validate_plugin_operation(
    std::string_view operation,
    const PluginLimits& limits);

[[nodiscard]] Result<void> validate_metadata(
    const PluginMetadata& metadata,
    const PluginLimits& limits);

}  // namespace iaisf::plugin
