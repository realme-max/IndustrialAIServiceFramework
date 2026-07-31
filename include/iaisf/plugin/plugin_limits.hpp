#pragma once

#include <cstddef>
#include <cstdint>

#include "iaisf/core/result.hpp"

namespace iaisf::plugin {

/**
 * Validated immutable limits for the in-process plugin registry.
 *
 * String lengths are measured as UTF-8 bytes stored in std::string. JSON
 * limits cover compact serialized bytes, depth, total value nodes, and bytes
 * per string/object key. Defaults are safety bounds, not throughput or
 * performance claims.
 */
class PluginLimits {
public:
    [[nodiscard]] static Result<PluginLimits> create(
        std::int64_t max_plugins = 128,
        std::int64_t max_operation_bytes = 128,
        std::int64_t max_name_bytes = 128,
        std::int64_t max_version_bytes = 64,
        std::int64_t max_description_bytes = 1024,
        std::int64_t max_error_message_bytes = 1024,
        std::int64_t max_input_bytes = 1024 * 1024,
        std::int64_t max_output_bytes = 4 * 1024 * 1024,
        std::int64_t max_json_depth = 64,
        std::int64_t max_json_elements = 100000,
        std::int64_t max_string_bytes = 1024 * 1024,
        std::int64_t max_capabilities = 64,
        std::int64_t max_capability_bytes = 128);

    [[nodiscard]] std::size_t max_plugins() const noexcept;
    [[nodiscard]] std::size_t max_operation_bytes() const noexcept;
    [[nodiscard]] std::size_t max_name_bytes() const noexcept;
    [[nodiscard]] std::size_t max_version_bytes() const noexcept;
    [[nodiscard]] std::size_t max_description_bytes() const noexcept;
    [[nodiscard]] std::size_t max_error_message_bytes() const noexcept;
    [[nodiscard]] std::size_t max_input_bytes() const noexcept;
    [[nodiscard]] std::size_t max_output_bytes() const noexcept;
    [[nodiscard]] std::size_t max_json_depth() const noexcept;
    [[nodiscard]] std::size_t max_json_elements() const noexcept;
    [[nodiscard]] std::size_t max_string_bytes() const noexcept;
    [[nodiscard]] std::size_t max_capabilities() const noexcept;
    [[nodiscard]] std::size_t max_capability_bytes() const noexcept;

private:
    PluginLimits(
        std::size_t max_plugins,
        std::size_t max_operation_bytes,
        std::size_t max_name_bytes,
        std::size_t max_version_bytes,
        std::size_t max_description_bytes,
        std::size_t max_error_message_bytes,
        std::size_t max_input_bytes,
        std::size_t max_output_bytes,
        std::size_t max_json_depth,
        std::size_t max_json_elements,
        std::size_t max_string_bytes,
        std::size_t max_capabilities,
        std::size_t max_capability_bytes) noexcept;

    std::size_t max_plugins_;
    std::size_t max_operation_bytes_;
    std::size_t max_name_bytes_;
    std::size_t max_version_bytes_;
    std::size_t max_description_bytes_;
    std::size_t max_error_message_bytes_;
    std::size_t max_input_bytes_;
    std::size_t max_output_bytes_;
    std::size_t max_json_depth_;
    std::size_t max_json_elements_;
    std::size_t max_string_bytes_;
    std::size_t max_capabilities_;
    std::size_t max_capability_bytes_;
};

}  // namespace iaisf::plugin
