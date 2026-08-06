#include "iaisf/plugin/abi/plugin_abi_validation.hpp"

#include <cstddef>
#include <new>
#include <string>
#include <utility>

#include "iaisf/core/error.hpp"
#include "iaisf/plugin/plugin_metadata.hpp"

namespace iaisf::plugin::abi {
namespace {

constexpr std::uint32_t minimum_size_for_api() noexcept {
    return static_cast<std::uint32_t>(
        offsetof(iaisf_plugin_api, destroy) +
        sizeof(((iaisf_plugin_api*)nullptr)->destroy));
}

constexpr std::uint32_t minimum_size_for_host() noexcept {
    return static_cast<std::uint32_t>(
        offsetof(iaisf_plugin_host_api, deallocate) +
        sizeof(((iaisf_plugin_host_api*)nullptr)->deallocate));
}

constexpr std::uint32_t minimum_size_for_metadata() noexcept {
    return static_cast<std::uint32_t>(
        offsetof(iaisf_plugin_metadata, capability_count) +
        sizeof(((iaisf_plugin_metadata*)nullptr)->capability_count));
}

Result<void> validate_header(
    const std::uint32_t abi_version,
    const std::uint32_t struct_size,
    const std::uint32_t minimum_size,
    const char* const subject) {
    if (abi_version != IAISF_PLUGIN_ABI_VERSION) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            std::string(subject) + " ABI version is unsupported"));
    }
    if (struct_size < minimum_size) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            std::string(subject) + " struct is smaller than its ABI prefix"));
    }
    return Result<void>::success();
}

}  // namespace

bool is_known_status(const iaisf_plugin_status_t status) noexcept {
    switch (status) {
    case IAISF_PLUGIN_STATUS_OK:
    case IAISF_PLUGIN_STATUS_INVALID_ARGUMENT:
    case IAISF_PLUGIN_STATUS_ABI_MISMATCH:
    case IAISF_PLUGIN_STATUS_STRUCT_TOO_SMALL:
    case IAISF_PLUGIN_STATUS_METADATA_INVALID:
    case IAISF_PLUGIN_STATUS_VALIDATION_FAILED:
    case IAISF_PLUGIN_STATUS_EXECUTION_FAILED:
    case IAISF_PLUGIN_STATUS_SHUTDOWN_FAILED:
    case IAISF_PLUGIN_STATUS_OUT_OF_MEMORY:
    case IAISF_PLUGIN_STATUS_INTERNAL_ERROR:
        return true;
    }
    return false;
}

Result<void> validate_status(const iaisf_plugin_status_t status) {
    if (is_known_status(status)) {
        return Result<void>::success();
    }
    return Result<void>::failure(make_error(
        ErrorCode::InternalError,
        "plugin ABI returned an unknown status code"));
}

Result<void> validate_api_response(
    const iaisf_plugin_status_t status,
    const std::uint32_t requested_abi_version,
    const std::uint32_t host_api_struct_size,
    const std::uint32_t output_api_capacity,
    const iaisf_plugin_api* const output_api) {
    auto status_valid = validate_status(status);
    if (!status_valid) {
        return status_valid;
    }
    if (status != IAISF_PLUGIN_STATUS_OK) {
        const auto code = status == IAISF_PLUGIN_STATUS_OUT_OF_MEMORY
                              ? ErrorCode::ResourceExhausted
                              : ErrorCode::InvalidState;
        return Result<void>::failure(make_error(
            code,
            "plugin ABI entry negotiation failed"));
    }
    if (output_api == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin ABI entry returned a null API pointer"));
    }
    if (requested_abi_version != IAISF_PLUGIN_ABI_VERSION ||
        host_api_struct_size < minimum_size_for_host()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin ABI entry received an unsupported host prefix"));
    }
    if (output_api_capacity < minimum_size_for_api()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin ABI entry output capacity is too small"));
    }
    if (output_api->struct_size > output_api_capacity) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin ABI entry reported a struct larger than its capacity"));
    }
    return validate_plugin_api(*output_api);
}

Result<void> validate_string_view(
    const iaisf_plugin_string_view value,
    const std::size_t maximum,
    const char* const field) {
    if (value.size > maximum) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " exceeds its byte limit"));
    }
    if (value.size != 0U && value.data == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " has a null data pointer"));
    }
    return Result<void>::success();
}

Result<void> validate_host_api(const iaisf_plugin_host_api& host) {
    auto header = validate_header(
        host.abi_version,
        host.struct_size,
        minimum_size_for_host(),
        "plugin host API");
    if (!header) {
        return header;
    }
    if (host.log == nullptr || host.allocate == nullptr ||
        host.deallocate == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin host API has a required callback missing"));
    }
    return Result<void>::success();
}

Result<void> validate_plugin_api(const iaisf_plugin_api& api) {
    auto header = validate_header(
        api.abi_version,
        api.struct_size,
        minimum_size_for_api(),
        "plugin API");
    if (!header) {
        return header;
    }
    if (api.get_metadata == nullptr || api.create == nullptr ||
        api.validate == nullptr || api.execute == nullptr ||
        api.shutdown == nullptr || api.destroy == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin API has a required callback missing"));
    }
    return Result<void>::success();
}

Result<void> validate_bytes_view(
    const iaisf_plugin_bytes_view value,
    const std::size_t maximum,
    const char* const field) {
    if (value.size > maximum) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " exceeds its byte limit"));
    }
    if (value.size != 0U && value.data == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " has a null data pointer"));
    }
    return Result<void>::success();
}

Result<void> validate_metadata_view(
    const iaisf_plugin_metadata& metadata,
    const PluginLimits& limits) {
    auto header = validate_header(
        metadata.abi_version,
        metadata.struct_size,
        minimum_size_for_metadata(),
        "plugin metadata");
    if (!header) {
        return header;
    }
    if ((metadata.flags & ~IAISF_PLUGIN_METADATA_FLAG_MOCK) != 0U) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin metadata has unsupported flags"));
    }
    auto operation = validate_string_view(
        metadata.operation, limits.max_operation_bytes(), "plugin operation");
    if (!operation) {
        return operation;
    }
    auto name = validate_string_view(
        metadata.name, limits.max_name_bytes(), "plugin name");
    if (!name) {
        return name;
    }
    auto version = validate_string_view(
        metadata.version, limits.max_version_bytes(), "plugin version");
    if (!version) {
        return version;
    }
    auto description = validate_string_view(
        metadata.description,
        limits.max_description_bytes(),
        "plugin description");
    if (!description) {
        return description;
    }
    if (metadata.operation.size == 0U || metadata.name.size == 0U ||
        metadata.version.size == 0U || metadata.description.size == 0U) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin metadata contains an empty required field"));
    }
    if (metadata.capability_count > limits.max_capabilities() ||
        (metadata.capability_count != 0U && metadata.capabilities == nullptr)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin metadata capabilities are invalid"));
    }
    for (std::size_t index = 0U; index < metadata.capability_count; ++index) {
        auto capability = validate_string_view(
            metadata.capabilities[index],
            limits.max_capability_bytes(),
            "plugin capability");
        if (!capability) {
            return capability;
        }
    }

    try {
        PluginMetadata converted;
        if (metadata.operation.size != 0U) {
            converted.operation.assign(
                metadata.operation.data, metadata.operation.size);
        }
        if (metadata.name.size != 0U) {
            converted.name.assign(metadata.name.data, metadata.name.size);
        }
        if (metadata.version.size != 0U) {
            converted.version.assign(
                metadata.version.data, metadata.version.size);
        }
        if (metadata.description.size != 0U) {
            converted.description.assign(
                metadata.description.data, metadata.description.size);
        }
        converted.mock =
            (metadata.flags & IAISF_PLUGIN_METADATA_FLAG_MOCK) != 0U;
        converted.capabilities.reserve(metadata.capability_count);
        for (std::size_t index = 0U;
             index < metadata.capability_count;
             ++index) {
            if (metadata.capabilities[index].size == 0U) {
                converted.capabilities.emplace_back();
            } else {
                converted.capabilities.emplace_back(
                    metadata.capabilities[index].data,
                    metadata.capabilities[index].size);
            }
        }
        return plugin::validate_metadata(converted, limits);
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "plugin metadata validation allocation failed"));
    } catch (...) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin metadata validation failed"));
    }
}

}  // namespace iaisf::plugin::abi
