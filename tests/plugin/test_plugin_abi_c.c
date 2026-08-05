#include "iaisf/plugin/abi/plugin_abi.h"

int main(void) {
    iaisf_plugin_metadata metadata;
    iaisf_plugin_api api;
    metadata.abi_version = IAISF_PLUGIN_ABI_VERSION;
    metadata.struct_size = (uint32_t)sizeof(metadata);
    api.abi_version = IAISF_PLUGIN_ABI_VERSION;
    api.struct_size = (uint32_t)sizeof(api);
    return (metadata.abi_version == api.abi_version) ? 0 : 1;
}
