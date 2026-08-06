#include "iaisf/plugin/abi/plugin_abi_adapter.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "iaisf/core/error.hpp"
#include "iaisf/plugin/abi/plugin_abi_validation.hpp"
#include "iaisf/plugin/plugin_metadata.hpp"

namespace iaisf::plugin::abi {
namespace {

constexpr std::size_t initialize_field_end() noexcept {
    return offsetof(iaisf_plugin_api, initialize) +
           sizeof(((iaisf_plugin_api*)nullptr)->initialize);
}

bool has_initialize_callback(const iaisf_plugin_api& api) noexcept {
    return api.struct_size >= initialize_field_end() &&
           api.initialize != nullptr;
}

Result<std::string> serialize_config(
    const nlohmann::json& config,
    const PluginLimits& limits) {
    try {
        auto storage = config.dump();
        if (storage.size() > limits.max_input_bytes()) {
            return Result<std::string>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "plugin configuration exceeds configured byte limit"));
        }
        return Result<std::string>::success(std::move(storage));
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "plugin configuration serialization allocation failed"));
    } catch (...) {
        return Result<std::string>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin configuration serialization failed"));
    }
}

struct OutputContext {
    std::string* output{nullptr};
    std::size_t maximum_bytes{0U};
};

Result<PluginMetadata> copy_metadata(const iaisf_plugin_metadata& value) {
    try {
        PluginMetadata metadata;
        if (value.operation.size != 0U) {
            metadata.operation.assign(value.operation.data, value.operation.size);
        }
        if (value.name.size != 0U) {
            metadata.name.assign(value.name.data, value.name.size);
        }
        if (value.version.size != 0U) {
            metadata.version.assign(value.version.data, value.version.size);
        }
        if (value.description.size != 0U) {
            metadata.description.assign(
                value.description.data, value.description.size);
        }
        metadata.mock = (value.flags & IAISF_PLUGIN_METADATA_FLAG_MOCK) != 0U;
        metadata.capabilities.reserve(value.capability_count);
        for (std::size_t index = 0U;
             index < value.capability_count;
             ++index) {
            if (value.capabilities[index].size == 0U) {
                metadata.capabilities.emplace_back();
            } else {
                metadata.capabilities.emplace_back(
                    value.capabilities[index].data,
                    value.capabilities[index].size);
            }
        }
        return Result<PluginMetadata>::success(std::move(metadata));
    } catch (const std::bad_alloc&) {
        return Result<PluginMetadata>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "plugin metadata copy allocation failed"));
    } catch (...) {
        return Result<PluginMetadata>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin metadata copy failed"));
    }
}

}  // namespace

Result<std::shared_ptr<AbiPluginAdapter>> AbiPluginAdapter::create(
    iaisf_plugin_api api,
    PluginLimits limits,
    nlohmann::json config) {
    auto api_valid = validate_plugin_api(api);
    if (!api_valid) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(
            std::move(api_valid).error());
    }

    auto config_storage = serialize_config(config, limits);
    if (!config_storage) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(
            std::move(config_storage).error());
    }
    const bool config_is_empty_object = config.is_object() && config.empty();
    if (!config_is_empty_object && !has_initialize_callback(api)) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(make_error(
            ErrorCode::InvalidState,
            "non-empty plugin configuration requires initialize callback"));
    }
    if (!has_initialize_callback(api)) {
        // The caller may have supplied only the v1 prefix.  Never inspect the
        // optional tail unless struct_size proves that it is present.
        api.initialize = nullptr;
    }

    iaisf_plugin_metadata metadata{};
    metadata.abi_version = IAISF_PLUGIN_ABI_VERSION;
    metadata.struct_size = sizeof(metadata);
    iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    try {
        status = api.get_metadata(api.plugin_context, &metadata);
    } catch (...) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(make_error(
            ErrorCode::InternalError,
            "plugin metadata callback failed"));
    }
    auto status_valid = validate_status(status);
    if (!status_valid) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(
            std::move(status_valid).error());
    }
    if (status != IAISF_PLUGIN_STATUS_OK) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin metadata callback returned an error"));
    }
    auto metadata_valid = validate_metadata_view(metadata, limits);
    if (!metadata_valid) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(
            std::move(metadata_valid).error());
    }
    auto metadata_copy = copy_metadata(metadata);
    if (!metadata_copy) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(
            std::move(metadata_copy).error());
    }

    std::shared_ptr<AbiPluginAdapter> adapter;
    try {
        adapter = std::shared_ptr<AbiPluginAdapter>(new AbiPluginAdapter(
            api,
            std::move(limits),
            std::move(metadata_copy).value(),
            nullptr,
            std::move(config_storage).value()));
        status = api.create(
            api.plugin_context, &adapter->host_, &adapter->instance_);
    } catch (...) {
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(make_error(
            ErrorCode::InternalError,
            "plugin create callback failed"));
    }
    if (status != IAISF_PLUGIN_STATUS_OK || adapter->instance_ == nullptr) {
        if (adapter && adapter->instance_ != nullptr) {
            (void)adapter->destroy_instance_locked();
        }
        return Result<std::shared_ptr<AbiPluginAdapter>>::failure(make_error(
            status == IAISF_PLUGIN_STATUS_OUT_OF_MEMORY
                ? ErrorCode::ResourceExhausted
                : ErrorCode::InternalError,
            "plugin create callback failed"));
    }
    return Result<std::shared_ptr<AbiPluginAdapter>>::success(std::move(adapter));
}

AbiPluginAdapter::AbiPluginAdapter(
    iaisf_plugin_api api,
    PluginLimits limits,
    PluginMetadata metadata,
    iaisf_plugin_handle* instance,
    std::string config_storage) noexcept
    : api_(api),
      limits_(std::move(limits)),
      metadata_(std::move(metadata)),
      instance_(instance),
      config_storage_(std::move(config_storage)),
      initialize_supported_(has_initialize_callback(api)) {
    host_.abi_version = IAISF_PLUGIN_ABI_VERSION;
    host_.struct_size = sizeof(host_);
    host_.log = &AbiPluginAdapter::default_log;
    host_.allocate = &AbiPluginAdapter::default_allocate;
    host_.deallocate = &AbiPluginAdapter::default_deallocate;
}

AbiPluginAdapter::~AbiPluginAdapter() noexcept {
    (void)shutdown();
}

PluginMetadata AbiPluginAdapter::metadata() const {
    return metadata_;
}

Result<void> AbiPluginAdapter::initialize() {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (instance_ == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin ABI instance is unavailable"));
    }
    if (shutdown_called_ || destroy_called_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin ABI instance is stopped"));
    }
    if (initialized_) {
        return Result<void>::success();
    }
    if (initialize_supported_) {
        const iaisf_plugin_bytes_view config{
            reinterpret_cast<const uint8_t*>(config_storage_.data()),
            config_storage_.size()};
        iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
        try {
            status = api_.initialize(instance_, config);
        } catch (...) {
            return Result<void>::failure(make_error(
                ErrorCode::InternalError,
                "plugin initialize callback failed"));
        }
        auto mapped = map_status(
            status, ErrorCode::InternalError, "plugin initialize failed");
        if (!mapped) {
            return mapped;
        }
    }
    initialized_ = true;
    return Result<void>::success();
}

Result<void> AbiPluginAdapter::validate_input(const nlohmann::json& input) const {
    std::string storage;
    auto serialized = serialize_input(input, storage);
    if (!serialized) {
        return Result<void>::failure(std::move(serialized).error());
    }
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (instance_ == nullptr || shutdown_called_ || destroy_called_ ||
        (initialize_supported_ && !initialized_)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin ABI instance is stopped"));
    }
    iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    try {
        status = api_.validate(instance_, serialized.value());
    } catch (...) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "plugin validation callback failed"));
    }
    return map_status(
        status, ErrorCode::InvalidArgument, "plugin validation failed");
}

Result<nlohmann::json> AbiPluginAdapter::execute(
    const nlohmann::json& input) const {
    std::string storage;
    auto serialized = serialize_input(input, storage);
    if (!serialized) {
        return Result<nlohmann::json>::failure(std::move(serialized).error());
    }
    std::string output;
    OutputContext output_context{&output, limits_.max_output_bytes()};
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (instance_ == nullptr || shutdown_called_ || destroy_called_ ||
        (initialize_supported_ && !initialized_)) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InvalidState,
            "plugin ABI instance is stopped"));
    }
    iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    try {
        status = api_.execute(
            instance_,
            serialized.value(),
            &AbiPluginAdapter::collect_output,
            &output_context);
    } catch (...) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InternalError,
            "plugin execute callback failed"));
    }
    auto mapped = map_status(
        status, ErrorCode::InternalError, "plugin execute failed");
    if (!mapped) {
        return Result<nlohmann::json>::failure(std::move(mapped).error());
    }
    try {
        return Result<nlohmann::json>::success(nlohmann::json::parse(output));
    } catch (const std::bad_alloc&) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "plugin output allocation failed"));
    } catch (...) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InternalError,
            "plugin execute returned invalid JSON"));
    }
}

Result<void> AbiPluginAdapter::shutdown() {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (destroy_called_ || instance_ == nullptr) {
        return Result<void>::success();
    }
    if (initialize_supported_ && !initialized_) {
        if (!destroy_instance_locked()) {
            return Result<void>::failure(make_error(
                ErrorCode::InternalError,
                "plugin destroy callback failed"));
        }
        return Result<void>::success();
    }
    if (shutdown_called_) {
        if (!destroy_instance_locked()) {
            return Result<void>::failure(make_error(
                ErrorCode::InternalError,
                "plugin destroy callback failed"));
        }
        return Result<void>::success();
    }
    shutdown_called_ = true;
    iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    try {
        status = api_.shutdown(instance_);
    } catch (...) {
        auto mapped = Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "plugin shutdown callback failed"));
        const bool destroy_succeeded = destroy_instance_locked();
        if (!destroy_succeeded) {
            return Result<void>::failure(make_error(
                ErrorCode::InternalError,
                "plugin destroy callback failed"));
        }
        return mapped;
    }
    auto mapped = map_status(
        status, ErrorCode::InternalError, "plugin shutdown failed");
    const bool destroy_succeeded = destroy_instance_locked();
    if (!mapped) {
        return mapped;
    }
    if (!destroy_succeeded) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "plugin destroy callback failed"));
    }
    return Result<void>::success();
}

bool AbiPluginAdapter::destroy_instance_locked() noexcept {
    if (destroy_called_ || instance_ == nullptr) {
        instance_ = nullptr;
        destroy_called_ = true;
        return true;
    }
    bool succeeded = true;
    try {
        api_.destroy(instance_);
    } catch (...) {
        succeeded = false;
    }
    instance_ = nullptr;
    destroy_called_ = true;
    return succeeded;
}

void AbiPluginAdapter::default_log(
    void* const context,
    const uint32_t level,
    const iaisf_plugin_string_view message) noexcept {
    (void)context;
    (void)level;
    (void)message;
}

void* AbiPluginAdapter::default_allocate(
    void* const context,
    const size_t size) noexcept {
    (void)context;
    return std::malloc(size == 0U ? 1U : size);
}

void AbiPluginAdapter::default_deallocate(
    void* const context,
    void* const memory) noexcept {
    (void)context;
    std::free(memory);
}

iaisf_plugin_status_t AbiPluginAdapter::collect_output(
    void* const context,
    const iaisf_plugin_bytes_view chunk) noexcept {
    if (context == nullptr || (chunk.size != 0U && chunk.data == nullptr)) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    auto* const output_context = static_cast<OutputContext*>(context);
    if (output_context->output == nullptr ||
        chunk.size > output_context->maximum_bytes ||
        output_context->output->size() >
            output_context->maximum_bytes - chunk.size) {
        return IAISF_PLUGIN_STATUS_OUT_OF_MEMORY;
    }
    if (chunk.size == 0U) {
        return IAISF_PLUGIN_STATUS_OK;
    }
    try {
        output_context->output->append(
            reinterpret_cast<const char*>(chunk.data), chunk.size);
    } catch (const std::bad_alloc&) {
        return IAISF_PLUGIN_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    }
    return IAISF_PLUGIN_STATUS_OK;
}

Result<iaisf_plugin_bytes_view> AbiPluginAdapter::serialize_input(
    const nlohmann::json& input,
    std::string& storage) const {
    try {
        storage = input.dump();
        if (storage.size() > limits_.max_input_bytes()) {
            return Result<iaisf_plugin_bytes_view>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "plugin input exceeds configured byte limit"));
        }
        iaisf_plugin_bytes_view view{};
        view.data = reinterpret_cast<const uint8_t*>(storage.data());
        view.size = storage.size();
        return Result<iaisf_plugin_bytes_view>::success(view);
    } catch (const std::bad_alloc&) {
        return Result<iaisf_plugin_bytes_view>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "plugin input serialization allocation failed"));
    } catch (...) {
        return Result<iaisf_plugin_bytes_view>::failure(make_error(
            ErrorCode::InvalidArgument,
            "plugin input serialization failed"));
    }
}

Result<void> AbiPluginAdapter::map_status(
    const iaisf_plugin_status_t status,
    const ErrorCode fallback,
    const char* const operation) const {
    auto status_valid = validate_status(status);
    if (!status_valid) {
        return status_valid;
    }
    if (status == IAISF_PLUGIN_STATUS_OK) {
        return Result<void>::success();
    }
    ErrorCode code = fallback;
    switch (status) {
    case IAISF_PLUGIN_STATUS_INVALID_ARGUMENT:
    case IAISF_PLUGIN_STATUS_VALIDATION_FAILED:
    case IAISF_PLUGIN_STATUS_METADATA_INVALID:
        code = ErrorCode::InvalidArgument;
        break;
    case IAISF_PLUGIN_STATUS_ABI_MISMATCH:
    case IAISF_PLUGIN_STATUS_STRUCT_TOO_SMALL:
        code = ErrorCode::InvalidState;
        break;
    case IAISF_PLUGIN_STATUS_OUT_OF_MEMORY:
        code = ErrorCode::ResourceExhausted;
        break;
    case IAISF_PLUGIN_STATUS_EXECUTION_FAILED:
    case IAISF_PLUGIN_STATUS_SHUTDOWN_FAILED:
    case IAISF_PLUGIN_STATUS_INTERNAL_ERROR:
        code = fallback;
        break;
    case IAISF_PLUGIN_STATUS_OK:
        break;
    }
    return Result<void>::failure(make_error(code, operation));
}

}  // namespace iaisf::plugin::abi
