#include "iaisf/plugin/abi/plugin_abi.h"

#include <cstring>
#include <new>

namespace {

constexpr char kOperation[] = "dynamic_fixture";
constexpr char kName[] = "Dynamic fixture plugin";
constexpr char kVersion[] = "1.0.0";
constexpr char kDescription[] = "C ABI integration fixture";
constexpr char kOutput[] = "{\"fixture\":true}";

struct FixtureInstance {
    const iaisf_plugin_host_api* host{nullptr};
    bool initialized{false};
};

iaisf_plugin_status_t IAISF_PLUGIN_CALL get_metadata(
    void* const context,
    iaisf_plugin_metadata* const metadata) {
    (void)context;
    if (metadata == nullptr || metadata->struct_size < sizeof(*metadata)) {
        return IAISF_PLUGIN_STATUS_STRUCT_TOO_SMALL;
    }
    metadata->abi_version = IAISF_PLUGIN_ABI_VERSION;
    metadata->operation = {kOperation, sizeof(kOperation) - 1U};
    metadata->name = {kName, sizeof(kName) - 1U};
    metadata->version = {kVersion, sizeof(kVersion) - 1U};
    metadata->description = {kDescription, sizeof(kDescription) - 1U};
    metadata->flags = 0U;
    metadata->reserved = 0U;
    metadata->capabilities = nullptr;
    metadata->capability_count = 0U;
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL create_instance(
    void* const context,
    const iaisf_plugin_host_api* const host,
    iaisf_plugin_handle** const instance) {
    (void)context;
    if (host == nullptr || instance == nullptr || host->allocate == nullptr) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    auto* const memory = host->allocate(host->context, sizeof(FixtureInstance));
    if (memory == nullptr) {
        return IAISF_PLUGIN_STATUS_OUT_OF_MEMORY;
    }
    *instance = reinterpret_cast<iaisf_plugin_handle*>(
        new (memory) FixtureInstance{host, false});
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL validate_instance(
    iaisf_plugin_handle* const instance,
    const iaisf_plugin_bytes_view input) {
    if (instance == nullptr || (input.size != 0U && input.data == nullptr)) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL execute_instance(
    iaisf_plugin_handle* const instance,
    const iaisf_plugin_bytes_view input,
    const iaisf_plugin_output_fn output,
    void* const output_context) {
    if (instance == nullptr || output == nullptr ||
        (input.size != 0U && input.data == nullptr)) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    const iaisf_plugin_bytes_view bytes{
        reinterpret_cast<const uint8_t*>(kOutput), sizeof(kOutput) - 1U};
    return output(output_context, bytes);
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL initialize_instance(
    iaisf_plugin_handle* const instance,
    const iaisf_plugin_bytes_view config) {
    if (instance == nullptr || (config.size != 0U && config.data == nullptr)) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    auto* const value = reinterpret_cast<FixtureInstance*>(instance);
    value->initialized = true;
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL shutdown_instance(
    iaisf_plugin_handle* const instance) {
    if (instance == nullptr) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    return IAISF_PLUGIN_STATUS_OK;
}

void IAISF_PLUGIN_CALL destroy_instance(iaisf_plugin_handle* const instance) {
    if (instance == nullptr) {
        return;
    }
    auto* const value = reinterpret_cast<FixtureInstance*>(instance);
    const auto* const host = value->host;
    value->~FixtureInstance();
    if (host != nullptr && host->deallocate != nullptr) {
        host->deallocate(host->context, value);
    }
}

}  // namespace

IAISF_PLUGIN_EXPORT iaisf_plugin_status_t IAISF_PLUGIN_CALL
iaisf_plugin_get_api_v1(
    const uint32_t requested_abi_version,
    const uint32_t host_api_struct_size,
    const uint32_t output_api_capacity,
    iaisf_plugin_api* const output_api) {
    if (output_api == nullptr ||
        requested_abi_version != IAISF_PLUGIN_ABI_VERSION ||
        host_api_struct_size < sizeof(iaisf_plugin_host_api) ||
        output_api_capacity < sizeof(iaisf_plugin_api)) {
        return IAISF_PLUGIN_STATUS_ABI_MISMATCH;
    }
    std::memset(output_api, 0, sizeof(*output_api));
    output_api->abi_version = IAISF_PLUGIN_ABI_VERSION;
    output_api->struct_size = sizeof(*output_api);
    output_api->plugin_context = nullptr;
    output_api->get_metadata = &get_metadata;
    output_api->create = &create_instance;
    output_api->validate = &validate_instance;
    output_api->execute = &execute_instance;
    output_api->shutdown = &shutdown_instance;
    output_api->destroy = &destroy_instance;
    output_api->initialize = &initialize_instance;
    return IAISF_PLUGIN_STATUS_OK;
}
