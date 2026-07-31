#include "iaisf/plugin/plugin_metadata.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "iaisf/core/error.hpp"

namespace iaisf::plugin {
namespace {

bool has_control_character(const std::string_view value) noexcept {
    for (const unsigned char byte : value) {
        if (byte < 0x20U || byte == 0x7FU) {
            return true;
        }
    }
    return false;
}

bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1;
            code_point = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (continuation_count > value.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto continuation =
                static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }

        const bool overlong =
            (continuation_count == 1 && code_point < 0x80U) ||
            (continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

Result<void> validate_text(
    const std::string_view value,
    const std::size_t maximum,
    const std::string_view field) {
    if (value.empty()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " must not be empty"));
    }
    if (value.size() > maximum) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " exceeds its byte limit"));
    }
    if (has_control_character(value) || !valid_utf8(value)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " contains invalid text"));
    }
    return Result<void>::success();
}

Result<void> validate_canonical_identifier(
    const std::string_view operation,
    const std::size_t maximum,
    const std::string_view field) {
    auto text = validate_text(operation, maximum, field);
    if (!text) {
        return text;
    }
    if (operation.front() == '.' || operation.back() == '.' ||
        operation.find("..") != std::string_view::npos) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " contains an empty segment"));
    }
    for (const unsigned char byte : operation) {
        const bool valid =
            (byte >= static_cast<unsigned char>('a') &&
             byte <= static_cast<unsigned char>('z')) ||
            (byte >= static_cast<unsigned char>('0') &&
             byte <= static_cast<unsigned char>('9')) ||
            byte == static_cast<unsigned char>('.') ||
            byte == static_cast<unsigned char>('_') ||
            byte == static_cast<unsigned char>('-');
        if (!valid) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                std::string(field) +
                    " must use canonical lowercase ASCII"));
        }
    }
    return Result<void>::success();
}

}  // namespace

Result<void> validate_plugin_operation(
    const std::string_view operation,
    const PluginLimits& limits) {
    return validate_canonical_identifier(
        operation,
        limits.max_operation_bytes(),
        "plugin operation");
}

Result<void> validate_metadata(
    const PluginMetadata& metadata,
    const PluginLimits& limits) {
    auto operation = validate_plugin_operation(metadata.operation, limits);
    if (!operation) {
        return operation;
    }
    auto name =
        validate_text(metadata.name, limits.max_name_bytes(), "plugin name");
    if (!name) {
        return name;
    }
    auto version = validate_text(
        metadata.version,
        limits.max_version_bytes(),
        "plugin version");
    if (!version) {
        return version;
    }
    auto description = validate_text(
        metadata.description,
        limits.max_description_bytes(),
        "plugin description");
    if (!description) {
        return description;
    }
    if (metadata.capabilities.size() > limits.max_capabilities()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin capabilities exceed the configured count limit"));
    }
    for (std::size_t index = 0; index < metadata.capabilities.size(); ++index) {
        auto capability = validate_canonical_identifier(
            metadata.capabilities[index],
            limits.max_capability_bytes(),
            "plugin capability");
        if (!capability) {
            return capability;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (metadata.capabilities[prior] == metadata.capabilities[index]) {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidArgument,
                    "plugin capabilities must be unique"));
            }
        }
    }
    return Result<void>::success();
}

}  // namespace iaisf::plugin
