#include "iaisf/plugin/abi/plugin_abi.h"

IAISF_PLUGIN_EXPORT iaisf_plugin_status_t IAISF_PLUGIN_CALL test_entry(
    uint32_t requested_abi_version,
    uint32_t host_api_struct_size,
    uint32_t output_api_capacity,
    iaisf_plugin_api* output_api) {
    (void)requested_abi_version;
    (void)host_api_struct_size;
    (void)output_api_capacity;
    (void)output_api;
    return IAISF_PLUGIN_STATUS_STRUCT_TOO_SMALL;
}

int main(void) {
    iaisf_plugin_metadata metadata;
    iaisf_plugin_api api;
    iaisf_plugin_get_api_v1_fn entry = &test_entry;
    metadata.abi_version = IAISF_PLUGIN_ABI_VERSION;
    metadata.struct_size = (uint32_t)sizeof(metadata);
    api.abi_version = IAISF_PLUGIN_ABI_VERSION;
    api.struct_size = (uint32_t)sizeof(api);
    return (metadata.abi_version == api.abi_version &&
            entry != 0 && IAISF_PLUGIN_ENTRY_SYMBOL_V1[0] != '\0')
               ? 0
               : 1;
}
