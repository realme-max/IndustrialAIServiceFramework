#include "iaisf/plugin/plugin_limits.hpp"

#include <array>
#include <limits>
#include <string_view>

#include "iaisf/core/error.hpp"

namespace iaisf::plugin {
namespace {

struct LimitInput {
    std::int64_t value;
    std::int64_t hard_maximum;
    std::string_view name;
};

constexpr std::int64_t kMaxPluginsHard = 100000;
constexpr std::int64_t kMaxOperationBytesHard = 4096;
constexpr std::int64_t kMaxNameBytesHard = 4096;
constexpr std::int64_t kMaxVersionBytesHard = 1024;
constexpr std::int64_t kMaxDescriptionBytesHard = 65536;
constexpr std::int64_t kMaxErrorMessageBytesHard = 65536;
constexpr std::int64_t kMaxJsonBytesHard = 64 * 1024 * 1024;
constexpr std::int64_t kMaxJsonDepthHard = 256;
constexpr std::int64_t kMaxJsonElementsHard = 1000000;
constexpr std::int64_t kMaxStringBytesHard = 16 * 1024 * 1024;
constexpr std::int64_t kMaxCapabilitiesHard = 1024;
constexpr std::int64_t kMaxCapabilityBytesHard = 4096;

Result<void> validate_limit(const LimitInput& input) {
    if (input.value <= 0) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(input.name) + " must be greater than zero"));
    }
    if (input.value > input.hard_maximum) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(input.name) + " exceeds its hard maximum"));
    }
    if (static_cast<std::uint64_t>(input.value) >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(input.name) + " exceeds the platform size limit"));
    }
    return Result<void>::success();
}

}  // namespace

Result<PluginLimits> PluginLimits::create(
    const std::int64_t max_plugins,
    const std::int64_t max_operation_bytes,
    const std::int64_t max_name_bytes,
    const std::int64_t max_version_bytes,
    const std::int64_t max_description_bytes,
    const std::int64_t max_error_message_bytes,
    const std::int64_t max_input_bytes,
    const std::int64_t max_output_bytes,
    const std::int64_t max_json_depth,
    const std::int64_t max_json_elements,
    const std::int64_t max_string_bytes,
    const std::int64_t max_capabilities,
    const std::int64_t max_capability_bytes) {
    const std::array<LimitInput, 13> inputs{{
        {max_plugins, kMaxPluginsHard, "max_plugins"},
        {max_operation_bytes, kMaxOperationBytesHard, "max_operation_bytes"},
        {max_name_bytes, kMaxNameBytesHard, "max_name_bytes"},
        {max_version_bytes, kMaxVersionBytesHard, "max_version_bytes"},
        {
            max_description_bytes,
            kMaxDescriptionBytesHard,
            "max_description_bytes",
        },
        {
            max_error_message_bytes,
            kMaxErrorMessageBytesHard,
            "max_error_message_bytes",
        },
        {max_input_bytes, kMaxJsonBytesHard, "max_input_bytes"},
        {max_output_bytes, kMaxJsonBytesHard, "max_output_bytes"},
        {max_json_depth, kMaxJsonDepthHard, "max_json_depth"},
        {max_json_elements, kMaxJsonElementsHard, "max_json_elements"},
        {max_string_bytes, kMaxStringBytesHard, "max_string_bytes"},
        {max_capabilities, kMaxCapabilitiesHard, "max_capabilities"},
        {
            max_capability_bytes,
            kMaxCapabilityBytesHard,
            "max_capability_bytes",
        },
    }};
    for (const auto& input : inputs) {
        auto valid = validate_limit(input);
        if (!valid) {
            return Result<PluginLimits>::failure(std::move(valid).error());
        }
    }

    return Result<PluginLimits>::success(PluginLimits{
        static_cast<std::size_t>(max_plugins),
        static_cast<std::size_t>(max_operation_bytes),
        static_cast<std::size_t>(max_name_bytes),
        static_cast<std::size_t>(max_version_bytes),
        static_cast<std::size_t>(max_description_bytes),
        static_cast<std::size_t>(max_error_message_bytes),
        static_cast<std::size_t>(max_input_bytes),
        static_cast<std::size_t>(max_output_bytes),
        static_cast<std::size_t>(max_json_depth),
        static_cast<std::size_t>(max_json_elements),
        static_cast<std::size_t>(max_string_bytes),
        static_cast<std::size_t>(max_capabilities),
        static_cast<std::size_t>(max_capability_bytes),
    });
}

PluginLimits::PluginLimits(
    const std::size_t max_plugins,
    const std::size_t max_operation_bytes,
    const std::size_t max_name_bytes,
    const std::size_t max_version_bytes,
    const std::size_t max_description_bytes,
    const std::size_t max_error_message_bytes,
    const std::size_t max_input_bytes,
    const std::size_t max_output_bytes,
    const std::size_t max_json_depth,
    const std::size_t max_json_elements,
    const std::size_t max_string_bytes,
    const std::size_t max_capabilities,
    const std::size_t max_capability_bytes) noexcept
    : max_plugins_(max_plugins),
      max_operation_bytes_(max_operation_bytes),
      max_name_bytes_(max_name_bytes),
      max_version_bytes_(max_version_bytes),
      max_description_bytes_(max_description_bytes),
      max_error_message_bytes_(max_error_message_bytes),
      max_input_bytes_(max_input_bytes),
      max_output_bytes_(max_output_bytes),
      max_json_depth_(max_json_depth),
      max_json_elements_(max_json_elements),
      max_string_bytes_(max_string_bytes),
      max_capabilities_(max_capabilities),
      max_capability_bytes_(max_capability_bytes) {}

std::size_t PluginLimits::max_plugins() const noexcept {
    return max_plugins_;
}

std::size_t PluginLimits::max_operation_bytes() const noexcept {
    return max_operation_bytes_;
}

std::size_t PluginLimits::max_name_bytes() const noexcept {
    return max_name_bytes_;
}

std::size_t PluginLimits::max_version_bytes() const noexcept {
    return max_version_bytes_;
}

std::size_t PluginLimits::max_description_bytes() const noexcept {
    return max_description_bytes_;
}

std::size_t PluginLimits::max_error_message_bytes() const noexcept {
    return max_error_message_bytes_;
}

std::size_t PluginLimits::max_input_bytes() const noexcept {
    return max_input_bytes_;
}

std::size_t PluginLimits::max_output_bytes() const noexcept {
    return max_output_bytes_;
}

std::size_t PluginLimits::max_json_depth() const noexcept {
    return max_json_depth_;
}

std::size_t PluginLimits::max_json_elements() const noexcept {
    return max_json_elements_;
}

std::size_t PluginLimits::max_string_bytes() const noexcept {
    return max_string_bytes_;
}

std::size_t PluginLimits::max_capabilities() const noexcept {
    return max_capabilities_;
}

std::size_t PluginLimits::max_capability_bytes() const noexcept {
    return max_capability_bytes_;
}

}  // namespace iaisf::plugin
