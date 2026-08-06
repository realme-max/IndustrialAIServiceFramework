#pragma once

#include <cstddef>
#include <cstdint>

#include "iaisf/core/result.hpp"
#include "iaisf/plugin/abi/plugin_abi.h"
#include "iaisf/plugin/plugin_limits.hpp"

namespace iaisf::plugin::abi {

[[nodiscard]] bool is_known_status(
    iaisf_plugin_status_t status) noexcept;

[[nodiscard]] Result<void> validate_status(
    iaisf_plugin_status_t status);

/** Validates a fixed entry-point response without reading beyond its prefix. */
[[nodiscard]] Result<void> validate_api_response(
    iaisf_plugin_status_t status,
    std::uint32_t requested_abi_version,
    std::uint32_t host_api_struct_size,
    std::uint32_t output_api_capacity,
    const iaisf_plugin_api* output_api);

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

[[nodiscard]] Result<void> validate_bytes_view(
    iaisf_plugin_bytes_view value,
    std::size_t maximum,
    const char* field);

}  // namespace iaisf::plugin::abi
