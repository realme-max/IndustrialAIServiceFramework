#pragma once

#include "iaisf/core/result.hpp"
#include "iaisf/plugin/abi/plugin_abi.h"
#include "iaisf/plugin/plugin_limits.hpp"

namespace iaisf::plugin::abi {

[[nodiscard]] Result<void> validate_host_api(
    const iaisf_plugin_host_api& host);

[[nodiscard]] Result<void> validate_plugin_api(
    const iaisf_plugin_api& api);

[[nodiscard]] Result<void> validate_metadata_view(
    const iaisf_plugin_metadata& metadata,
    const PluginLimits& limits);

[[nodiscard]] Result<void> validate_string_view(
    iaisf_plugin_string_view value,
    std::size_t maximum,
    const char* field);

}  // namespace iaisf::plugin::abi
