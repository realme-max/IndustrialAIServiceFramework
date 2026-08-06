#include "iaisf/plugin/dynamic_plugin_adapter.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "iaisf/core/error.hpp"
#include "iaisf/core/json_value_limits.hpp"
#include "iaisf/plugin/abi/plugin_abi_validation.hpp"
#include "iaisf/plugin/detail/dynamic_module.hpp"
#include "iaisf/plugin/plugin_metadata.hpp"

namespace iaisf::plugin {
namespace {

constexpr std::size_t initialize_field_end() noexcept {
    return offsetof(iaisf_plugin_api, initialize) +
           sizeof(((iaisf_plugin_api*)nullptr)->initialize);
}

bool has_initialize_callback(const iaisf_plugin_api& api) noexcept {
    return api.struct_size >= initialize_field_end() &&
           api.initialize != nullptr;
}

Error internal_error(const char* const message) {
    return make_error(ErrorCode::InternalError, message);
}

struct OutputContext {
    std::string* output{nullptr};
    std::size_t maximum_bytes{0U};
};

}  // namespace

Result<std::shared_ptr<DynamicPluginAdapter>>
DynamicPluginAdapter::create(
    std::shared_ptr<detail::DynamicModule> module,
    const iaisf_plugin_api& api,
    PluginMetadata metadata,
    nlohmann::json config) {
    auto limits = PluginLimits::create();
    if (!limits) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(limits).error());
    }
    return create(std::move(module), api, std::move(metadata),
                  std::move(limits).value(), std::move(config));
}

Result<std::shared_ptr<DynamicPluginAdapter>>
DynamicPluginAdapter::create(
    std::shared_ptr<detail::DynamicModule> module,
    const iaisf_plugin_api& api,
    PluginMetadata metadata,
    PluginLimits limits,
    nlohmann::json config) {
    if (!module || !module->loaded()) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            make_error(ErrorCode::InvalidState,
                       "dynamic plugin module is not loaded"));
    }
    auto api_valid = abi::validate_plugin_api(api);
    if (!api_valid) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(api_valid).error());
    }
    iaisf_plugin_api normalized_api = api;
    auto metadata_valid = validate_metadata(metadata, limits);
    if (!metadata_valid) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(metadata_valid).error());
    }
    try {
        auto config_storage = config.dump();
        if (config_storage.size() > limits.max_input_bytes()) {
            return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
                make_error(ErrorCode::ResourceExhausted,
                           "plugin configuration exceeds configured byte limit"));
        }
        const bool empty_config = config.is_object() && config.empty();
        if (!empty_config && !has_initialize_callback(normalized_api)) {
            return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
                make_error(ErrorCode::InvalidState,
                           "non-empty plugin configuration requires initialize callback"));
        }

        if (!has_initialize_callback(normalized_api)) {
            // The optional tail is not read unless struct_size proves it is
            // present, preserving compatibility with the v1 prefix.
            normalized_api.initialize = nullptr;
        }

        std::shared_ptr<DynamicPluginAdapter> adapter =
            std::shared_ptr<DynamicPluginAdapter>(new DynamicPluginAdapter(
                std::move(module), normalized_api, std::move(metadata),
                std::move(limits), std::move(config_storage)));
        iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
        try {
            status = adapter->api_.create(
                adapter->api_.plugin_context,
                &adapter->host_,
                &adapter->instance_);
        } catch (...) {
            if (adapter->instance_ != nullptr) {
                (void)adapter->destroy_instance_locked();
            }
            return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
                internal_error("plugin create callback failed"));
        }
        auto status_valid = abi::validate_status(status);
        if (!status_valid || status != IAISF_PLUGIN_STATUS_OK ||
            adapter->instance_ == nullptr) {
            if (adapter->instance_ != nullptr) {
                (void)adapter->destroy_instance_locked();
            }
            if (!status_valid) {
                return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
                    std::move(status_valid).error());
            }
            return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
                make_error(status == IAISF_PLUGIN_STATUS_OUT_OF_MEMORY
                               ? ErrorCode::ResourceExhausted
                               : ErrorCode::InternalError,
                           "plugin create callback failed"));
        }
        adapter->state_ = DynamicPluginAdapterState::Created;
        return Result<std::shared_ptr<DynamicPluginAdapter>>::success(
            std::move(adapter));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "dynamic plugin adapter allocation failed"));
    } catch (...) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            internal_error("dynamic plugin adapter creation failed"));
    }
}

DynamicPluginAdapter::DynamicPluginAdapter(
    std::shared_ptr<detail::DynamicModule> module,
    iaisf_plugin_api api,
    PluginMetadata metadata,
    PluginLimits limits,
    std::string config_storage) noexcept
    : module_(std::move(module)),
      api_(api),
      limits_(std::move(limits)),
      metadata_(std::move(metadata)),
      config_storage_(std::move(config_storage)),
      initialize_supported_(has_initialize_callback(api)) {
    host_.abi_version = IAISF_PLUGIN_ABI_VERSION;
    host_.struct_size = sizeof(host_);
    host_.log = &DynamicPluginAdapter::default_log;
    host_.allocate = &DynamicPluginAdapter::default_allocate;
    host_.deallocate = &DynamicPluginAdapter::default_deallocate;
}

DynamicPluginAdapter::~DynamicPluginAdapter() noexcept {
    try {
        (void)shutdown();
    } catch (...) {
        // Destruction cannot report an ABI failure.  The instance and module
        // are still released by the noexcept cleanup path below.
        std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
        (void)destroy_instance_locked();
        state_ = DynamicPluginAdapterState::Stopped;
    }
}

bool DynamicPluginAdapter::release_module() noexcept {
    std::unique_lock<std::shared_mutex> lock{lifecycle_mutex_};
    if (!module_) {
        return true;
    }
    const bool success = module_->close();
    module_.reset();
    return success;
}

PluginMetadata DynamicPluginAdapter::metadata() const {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return metadata_;
}

DynamicPluginAdapterState DynamicPluginAdapter::state() const noexcept {
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    return state_;
}

Result<void> DynamicPluginAdapter::initialize() {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (state_ == DynamicPluginAdapterState::Initialized) {
        return Result<void>::success();
    }
    if (state_ != DynamicPluginAdapterState::Created ||
        instance_ == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "dynamic plugin is not in the created state"));
    }
    if (initialize_supported_) {
        const iaisf_plugin_bytes_view config{
            reinterpret_cast<const uint8_t*>(config_storage_.data()),
            config_storage_.size()};
        iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
        try {
            status = api_.initialize(instance_, config);
        } catch (...) {
            (void)destroy_instance_locked();
            state_ = DynamicPluginAdapterState::Failed;
            return Result<void>::failure(
                internal_error("plugin initialize callback failed"));
        }
        auto mapped = map_status(
            status, ErrorCode::InternalError, "plugin initialize failed");
        if (!mapped) {
            (void)destroy_instance_locked();
            state_ = DynamicPluginAdapterState::Failed;
            return mapped;
        }
    }
    initialized_ = true;
    state_ = DynamicPluginAdapterState::Initialized;
    return Result<void>::success();
}

Result<void> DynamicPluginAdapter::validate_input(
    const nlohmann::json& input) const {
    std::string storage;
    auto serialized = serialize_input(input, storage);
    if (!serialized) {
        return Result<void>::failure(std::move(serialized).error());
    }
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (state_ != DynamicPluginAdapterState::Initialized ||
        instance_ == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState, "dynamic plugin is not initialized"));
    }
    iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    try {
        status = api_.validate(instance_, serialized.value());
    } catch (...) {
        return Result<void>::failure(
            internal_error("plugin validation callback failed"));
    }
    return map_status(
        status, ErrorCode::InvalidArgument, "plugin validation failed");
}

Result<nlohmann::json> DynamicPluginAdapter::execute(
    const nlohmann::json& input) const {
    std::string storage;
    auto serialized = serialize_input(input, storage);
    if (!serialized) {
        return Result<nlohmann::json>::failure(std::move(serialized).error());
    }
    std::string output;
    OutputContext output_context{&output, limits_.max_output_bytes()};
    std::shared_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (state_ != DynamicPluginAdapterState::Initialized ||
        instance_ == nullptr) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InvalidState, "dynamic plugin is not initialized"));
    }
    iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    try {
        status = api_.execute(
            instance_, serialized.value(), &DynamicPluginAdapter::collect_output,
            &output_context);
    } catch (...) {
        return Result<nlohmann::json>::failure(
            internal_error("plugin execute callback failed"));
    }
    auto mapped = map_status(
        status, ErrorCode::InternalError, "plugin execute failed");
    if (!mapped) {
        return Result<nlohmann::json>::failure(std::move(mapped).error());
    }
    try {
        auto result = nlohmann::json::parse(output);
        auto limits = validate_json_value(
            result,
            JsonValueLimits{
                limits_.max_output_bytes(), limits_.max_json_depth(),
                limits_.max_json_elements(), limits_.max_string_bytes()},
            "plugin output", ErrorCode::InternalError);
        if (!limits) {
            return Result<nlohmann::json>::failure(std::move(limits).error());
        }
        return Result<nlohmann::json>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::ResourceExhausted, "plugin output allocation failed"));
    } catch (...) {
        return Result<nlohmann::json>::failure(
            internal_error("plugin execute returned invalid JSON"));
    }
}

Result<void> DynamicPluginAdapter::shutdown() {
    std::unique_lock<std::shared_mutex> lock(lifecycle_mutex_);
    if (state_ == DynamicPluginAdapterState::Stopped) {
        if (shutdown_error_.has_value()) {
            return Result<void>::failure(*shutdown_error_);
        }
        return Result<void>::success();
    }
    if (state_ == DynamicPluginAdapterState::Draining) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState, "dynamic plugin shutdown is in progress"));
    }
    state_ = DynamicPluginAdapterState::Draining;
    std::optional<Error> first_error;
    if (initialized_ && !shutdown_called_ && instance_ != nullptr) {
        shutdown_called_ = true;
        iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
        try {
            status = api_.shutdown(instance_);
        } catch (...) {
            first_error = internal_error("plugin shutdown callback failed");
        }
        if (!first_error.has_value()) {
            auto mapped = map_status(
                status, ErrorCode::InternalError, "plugin shutdown failed");
            if (!mapped) {
                first_error = std::move(mapped).error();
            }
        }
    }
    if (!destroy_instance_locked()) {
        if (!first_error.has_value()) {
            first_error = internal_error("plugin destroy callback failed");
        }
    }
    state_ = DynamicPluginAdapterState::Stopped;
    if (first_error.has_value()) {
        shutdown_error_ = *first_error;
        return Result<void>::failure(std::move(*first_error));
    }
    return Result<void>::success();
}

bool DynamicPluginAdapter::destroy_instance_locked() noexcept {
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

void DynamicPluginAdapter::default_log(
    void* const context,
    const uint32_t level,
    const iaisf_plugin_string_view message) noexcept {
    (void)context;
    (void)level;
    (void)message;
}

void* DynamicPluginAdapter::default_allocate(
    void* const context,
    const size_t size) noexcept {
    (void)context;
    return std::malloc(size == 0U ? 1U : size);
}

void DynamicPluginAdapter::default_deallocate(
    void* const context,
    void* const memory) noexcept {
    (void)context;
    std::free(memory);
}

iaisf_plugin_status_t DynamicPluginAdapter::collect_output(
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
    try {
        if (chunk.size != 0U) {
            output_context->output->append(
                reinterpret_cast<const char*>(chunk.data), chunk.size);
        }
        return IAISF_PLUGIN_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return IAISF_PLUGIN_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    }
}

Result<iaisf_plugin_bytes_view> DynamicPluginAdapter::serialize_input(
    const nlohmann::json& input,
    std::string& storage) const {
    try {
        storage = input.dump();
        if (storage.size() > limits_.max_input_bytes()) {
            return Result<iaisf_plugin_bytes_view>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "plugin input exceeds configured byte limit"));
        }
        return Result<iaisf_plugin_bytes_view>::success(
            iaisf_plugin_bytes_view{
                reinterpret_cast<const uint8_t*>(storage.data()),
                storage.size()});
    } catch (const std::bad_alloc&) {
        return Result<iaisf_plugin_bytes_view>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "plugin input serialization allocation failed"));
    } catch (...) {
        return Result<iaisf_plugin_bytes_view>::failure(
            internal_error("plugin input serialization failed"));
    }
}

Result<void> DynamicPluginAdapter::map_status(
    const iaisf_plugin_status_t status,
    const ErrorCode fallback,
    const char* const operation) const {
    auto status_valid = abi::validate_status(status);
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

}  // namespace iaisf::plugin
