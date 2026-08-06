#ifndef IAISF_PLUGIN_ABI_H
#define IAISF_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(__cplusplus)
#define IAISF_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define IAISF_PLUGIN_EXPORT __declspec(dllexport)
#endif
#define IAISF_PLUGIN_CALL __cdecl
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__cplusplus)
#define IAISF_PLUGIN_EXPORT \
    extern "C" __attribute__((visibility("default")))
#else
#define IAISF_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
#define IAISF_PLUGIN_CALL
#else
#if defined(__cplusplus)
#define IAISF_PLUGIN_EXPORT extern "C"
#else
#define IAISF_PLUGIN_EXPORT
#endif
#define IAISF_PLUGIN_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define IAISF_PLUGIN_ABI_VERSION 1U
#define IAISF_PLUGIN_ENTRY_SYMBOL_V1 "iaisf_plugin_get_api_v1"
#define IAISF_PLUGIN_METADATA_FLAG_MOCK 1U

typedef uint32_t iaisf_plugin_status_t;

#define IAISF_PLUGIN_STATUS_OK ((iaisf_plugin_status_t)0U)
#define IAISF_PLUGIN_STATUS_INVALID_ARGUMENT ((iaisf_plugin_status_t)1U)
#define IAISF_PLUGIN_STATUS_ABI_MISMATCH ((iaisf_plugin_status_t)2U)
#define IAISF_PLUGIN_STATUS_STRUCT_TOO_SMALL ((iaisf_plugin_status_t)3U)
#define IAISF_PLUGIN_STATUS_METADATA_INVALID ((iaisf_plugin_status_t)4U)
#define IAISF_PLUGIN_STATUS_VALIDATION_FAILED ((iaisf_plugin_status_t)5U)
#define IAISF_PLUGIN_STATUS_EXECUTION_FAILED ((iaisf_plugin_status_t)6U)
#define IAISF_PLUGIN_STATUS_SHUTDOWN_FAILED ((iaisf_plugin_status_t)7U)
#define IAISF_PLUGIN_STATUS_OUT_OF_MEMORY ((iaisf_plugin_status_t)8U)
#define IAISF_PLUGIN_STATUS_INTERNAL_ERROR ((iaisf_plugin_status_t)9U)

typedef struct iaisf_plugin_string_view {
    const char* data;
    size_t size;
} iaisf_plugin_string_view;

typedef struct iaisf_plugin_bytes_view {
    const uint8_t* data;
    size_t size;
} iaisf_plugin_bytes_view;

typedef struct iaisf_plugin_handle iaisf_plugin_handle;

typedef struct iaisf_plugin_metadata {
    uint32_t abi_version;
    uint32_t struct_size;
    iaisf_plugin_string_view operation;
    iaisf_plugin_string_view name;
    iaisf_plugin_string_view version;
    iaisf_plugin_string_view description;
    uint32_t flags;
    uint32_t reserved;
    const iaisf_plugin_string_view* capabilities;
    size_t capability_count;
} iaisf_plugin_metadata;

typedef void (IAISF_PLUGIN_CALL *iaisf_plugin_log_fn)(
    void* context,
    uint32_t level,
    iaisf_plugin_string_view message);

typedef void* (IAISF_PLUGIN_CALL *iaisf_plugin_allocate_fn)(
    void* context,
    size_t size);
typedef void (IAISF_PLUGIN_CALL *iaisf_plugin_deallocate_fn)(
    void* context,
    void* memory);

typedef struct iaisf_plugin_host_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;
    iaisf_plugin_log_fn log;
    iaisf_plugin_allocate_fn allocate;
    iaisf_plugin_deallocate_fn deallocate;
} iaisf_plugin_host_api;

typedef iaisf_plugin_status_t (IAISF_PLUGIN_CALL *iaisf_plugin_get_metadata_fn)(
    void* plugin_context,
    iaisf_plugin_metadata* metadata);

typedef iaisf_plugin_status_t (IAISF_PLUGIN_CALL *iaisf_plugin_create_fn)(
    void* plugin_context,
    const iaisf_plugin_host_api* host,
    iaisf_plugin_handle** instance);

typedef iaisf_plugin_status_t (IAISF_PLUGIN_CALL *iaisf_plugin_validate_fn)(
    iaisf_plugin_handle* instance,
    iaisf_plugin_bytes_view input);

typedef iaisf_plugin_status_t (IAISF_PLUGIN_CALL *iaisf_plugin_output_fn)(
    void* context,
    iaisf_plugin_bytes_view chunk);

typedef iaisf_plugin_status_t (IAISF_PLUGIN_CALL *iaisf_plugin_execute_fn)(
    iaisf_plugin_handle* instance,
    iaisf_plugin_bytes_view input,
    iaisf_plugin_output_fn output,
    void* output_context);

typedef iaisf_plugin_status_t (IAISF_PLUGIN_CALL *iaisf_plugin_shutdown_fn)(
    iaisf_plugin_handle* instance);
typedef void (IAISF_PLUGIN_CALL *iaisf_plugin_destroy_fn)(
    iaisf_plugin_handle* instance);

/** Optional append-only lifecycle hook introduced after the v1 prefix. */
typedef iaisf_plugin_status_t (IAISF_PLUGIN_CALL *iaisf_plugin_initialize_fn)(
    iaisf_plugin_handle* instance,
    iaisf_plugin_bytes_view compact_config);

typedef struct iaisf_plugin_api {
    uint32_t abi_version;
    uint32_t struct_size;
    void* plugin_context;
    iaisf_plugin_get_metadata_fn get_metadata;
    iaisf_plugin_create_fn create;
    iaisf_plugin_validate_fn validate;
    iaisf_plugin_execute_fn execute;
    iaisf_plugin_shutdown_fn shutdown;
    iaisf_plugin_destroy_fn destroy;
    /* Optional when struct_size includes this field. */
    iaisf_plugin_initialize_fn initialize;
} iaisf_plugin_api;

typedef iaisf_plugin_status_t (IAISF_PLUGIN_CALL *
                               iaisf_plugin_get_api_v1_fn)(
    uint32_t requested_abi_version,
    uint32_t host_api_struct_size,
    uint32_t output_api_capacity,
    iaisf_plugin_api* output_api);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IAISF_PLUGIN_ABI_H */
