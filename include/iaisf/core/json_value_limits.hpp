#pragma once

#include <cstddef>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "iaisf/core/error.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf {

/**
 * Structural and serialized-size limits for an already parsed JSON value.
 *
 * Depth counts the root as one. Elements count every JSON value node,
 * including the root, while object member names are covered by the string
 * byte limit. The final serialized byte check uses nlohmann/json's compact
 * UTF-8 representation exactly and aborts serialization on the first byte
 * above the configured maximum.
 */
struct JsonValueLimits {
    std::size_t max_serialized_bytes;
    std::size_t max_depth;
    std::size_t max_elements;
    std::size_t max_string_bytes;
};

/**
 * Validates one in-memory JSON value without mutating it.
 *
 * Structural capacity failures use ResourceExhausted. Discarded values,
 * non-finite floating-point values, and invalid UTF-8 use invalid_value_code.
 * The returned value is the compact serialized byte count.
 */
[[nodiscard]] Result<std::size_t> validate_json_value(
    const nlohmann::json& value,
    const JsonValueLimits& limits,
    std::string_view value_name,
    ErrorCode invalid_value_code = ErrorCode::InvalidArgument);

}  // namespace iaisf
