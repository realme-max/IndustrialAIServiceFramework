#include "iaisf/plugin/detail/dynamic_plugin_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <new>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "iaisf/core/error.hpp"
#include "iaisf/plugin/abi/plugin_abi_validation.hpp"
#include "iaisf/plugin/dynamic_plugin_adapter.hpp"
#include "iaisf/plugin/detail/dynamic_module.hpp"
#include "iaisf/plugin/detail/plugin_safe_path.hpp"
#include "iaisf/plugin/plugin_metadata.hpp"
#include "iaisf/plugin/plugin_limits.hpp"

namespace iaisf::plugin::detail {
namespace {

bool extension_matches(const std::filesystem::path& path) {
    auto extension = path.extension().u8string();
#if defined(_WIN32)
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const char value) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(value)));
        });
    return extension == ".dll";
#else
    return extension == ".so";
#endif
}

Result<PluginMetadata> copy_metadata(
    const iaisf_plugin_metadata& value) {
    try {
        PluginMetadata metadata;
        metadata.operation.assign(value.operation.data, value.operation.size);
        metadata.name.assign(value.name.data, value.name.size);
        metadata.version.assign(value.version.data, value.version.size);
        metadata.description.assign(
            value.description.data, value.description.size);
        metadata.mock =
            (value.flags & IAISF_PLUGIN_METADATA_FLAG_MOCK) != 0U;
        metadata.capabilities.reserve(value.capability_count);
        for (std::size_t index = 0U;
             index < value.capability_count;
             ++index) {
            metadata.capabilities.emplace_back(
                value.capabilities[index].data,
                value.capabilities[index].size);
        }
        return Result<PluginMetadata>::success(std::move(metadata));
    } catch (const std::bad_alloc&) {
        return Result<PluginMetadata>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "dynamic plugin metadata allocation failed"));
    } catch (...) {
        return Result<PluginMetadata>::failure(make_error(
            ErrorCode::InternalError,
            "dynamic plugin metadata copy failed"));
    }
}

}  // namespace

Result<std::unique_ptr<DynamicPluginLoader>> DynamicPluginLoader::create(
    DynamicPluginLoaderOptions options) {
    auto root = SafePathResolver::canonical_root(options.plugin_root);
    if (!root) {
        return Result<std::unique_ptr<DynamicPluginLoader>>::failure(
            std::move(root).error());
    }
    try {
        return Result<std::unique_ptr<DynamicPluginLoader>>::success(
            std::unique_ptr<DynamicPluginLoader>(
                new DynamicPluginLoader(std::move(root).value())));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<DynamicPluginLoader>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "dynamic plugin loader allocation failed"));
    } catch (...) {
        return Result<std::unique_ptr<DynamicPluginLoader>>::failure(make_error(
            ErrorCode::InternalError,
            "dynamic plugin loader creation failed"));
    }
}

Result<std::shared_ptr<DynamicModule>> DynamicPluginLoader::load_module(
    const DynamicPluginModuleSpec& spec) {
    if (spec.id.empty()) {
        return Result<std::shared_ptr<DynamicModule>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "dynamic plugin module id is empty"));
    }
    auto resolved = SafePathResolver::resolve(plugin_root_, spec.library);
    if (!resolved) {
        return Result<std::shared_ptr<DynamicModule>>::failure(
            std::move(resolved).error());
    }
    if (!extension_matches(resolved.value())) {
        return Result<std::shared_ptr<DynamicModule>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "dynamic plugin library extension is unsupported"));
    }
    const auto& normalized = resolved.value();
    if (loaded_paths_.find(normalized) != loaded_paths_.end()) {
        return Result<std::shared_ptr<DynamicModule>>::failure(make_error(
            ErrorCode::InvalidState,
            "dynamic plugin library was already loaded"));
    }

    auto opened = DynamicModule::open(normalized);
    if (!opened) {
        return Result<std::shared_ptr<DynamicModule>>::failure(
            std::move(opened).error());
    }
    try {
        auto module = std::make_shared<DynamicModule>(std::move(opened).value());
        loaded_paths_.insert(normalized);
        return Result<std::shared_ptr<DynamicModule>>::success(std::move(module));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<DynamicModule>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "dynamic plugin module allocation failed"));
    } catch (...) {
        return Result<std::shared_ptr<DynamicModule>>::failure(make_error(
            ErrorCode::InternalError,
            "dynamic plugin module registration failed"));
    }
}

Result<std::shared_ptr<DynamicPluginAdapter>>
DynamicPluginLoader::load_plugin(const DynamicPluginModuleSpec& spec) {
    auto limits = PluginLimits::create();
    if (!limits) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(limits).error());
    }
    return load_plugin(spec, std::move(limits).value());
}

Result<std::shared_ptr<DynamicPluginAdapter>>
DynamicPluginLoader::load_plugin(
    const DynamicPluginModuleSpec& spec,
    const PluginLimits& limits) {
    // Resolve once before opening so every failure after load can undo the
    // loader's duplicate-path reservation.  A failed adapter must not poison
    // a loader that the caller chooses to reuse for another transaction.
    auto normalized_result = SafePathResolver::resolve(
        plugin_root_, spec.library);
    if (!normalized_result) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(normalized_result).error());
    }
    const auto normalized_path = normalized_result.value();
    const auto rollback_path = [this, &normalized_path]() noexcept {
        loaded_paths_.erase(normalized_path);
    };
    auto module_result = load_module(spec);
    if (!module_result) {
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(module_result).error());
    }
    auto module = std::move(module_result).value();
    auto symbol = module->resolve_symbol(IAISF_PLUGIN_ENTRY_SYMBOL_V1);
    if (!symbol) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(symbol).error());
    }

    iaisf_plugin_api api{};
    iaisf_plugin_status_t status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    try {
        const auto entry = reinterpret_cast<iaisf_plugin_get_api_v1_fn>(
            symbol.value());
        status = entry(
            IAISF_PLUGIN_ABI_VERSION,
            static_cast<std::uint32_t>(sizeof(iaisf_plugin_host_api)),
            static_cast<std::uint32_t>(sizeof(iaisf_plugin_api)),
            &api);
    } catch (...) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            make_error(ErrorCode::InternalError,
                       "dynamic plugin entry callback failed"));
    }
    auto response = abi::validate_api_response(
        status,
        IAISF_PLUGIN_ABI_VERSION,
        static_cast<std::uint32_t>(sizeof(iaisf_plugin_host_api)),
        static_cast<std::uint32_t>(sizeof(iaisf_plugin_api)),
        &api);
    if (!response) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(response).error());
    }

    nlohmann::json config = nlohmann::json::object();
    try {
        config = nlohmann::json::parse(spec.config_json);
    } catch (...) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            make_error(ErrorCode::InvalidArgument,
                       "dynamic plugin configuration is invalid JSON"));
    }
    iaisf_plugin_metadata raw_metadata{};
    raw_metadata.abi_version = IAISF_PLUGIN_ABI_VERSION;
    raw_metadata.struct_size = sizeof(raw_metadata);
    try {
        status = api.get_metadata(api.plugin_context, &raw_metadata);
    } catch (...) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            make_error(ErrorCode::InternalError,
                       "dynamic plugin metadata callback failed"));
    }
    auto status_valid = abi::validate_status(status);
    if (!status_valid) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(status_valid).error());
    }
    if (status != IAISF_PLUGIN_STATUS_OK) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            make_error(ErrorCode::InvalidArgument,
                       "dynamic plugin metadata callback failed"));
    }
    auto metadata_valid = abi::validate_metadata_view(
        raw_metadata, limits);
    if (!metadata_valid) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(metadata_valid).error());
    }
    auto metadata = copy_metadata(raw_metadata);
    if (!metadata) {
        rollback_path();
        return Result<std::shared_ptr<DynamicPluginAdapter>>::failure(
            std::move(metadata).error());
    }
    auto adapter = DynamicPluginAdapter::create(
        std::move(module), api, std::move(metadata).value(),
        limits, std::move(config));
    if (!adapter) {
        rollback_path();
    }
    return adapter;
}

DynamicPluginLoader::DynamicPluginLoader(
    std::filesystem::path plugin_root)
    : plugin_root_(std::move(plugin_root)) {}

const std::filesystem::path& DynamicPluginLoader::plugin_root() const noexcept {
    return plugin_root_;
}

}  // namespace iaisf::plugin::detail
