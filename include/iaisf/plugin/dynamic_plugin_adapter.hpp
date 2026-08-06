#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "iaisf/core/result.hpp"
#include "iaisf/plugin/abi/plugin_abi.h"
#include "iaisf/plugin/algorithm_plugin.hpp"
#include "iaisf/plugin/plugin_limits.hpp"

namespace iaisf::plugin::detail {
class DynamicModule;
}

namespace iaisf::plugin {

enum class DynamicPluginAdapterState {
    Prepared,
    Created,
    Initialized,
    Draining,
    Stopped,
    Failed,
};

/**
 * Adapts one negotiated C ABI plugin while retaining its module handle.
 *
 * The adapter owns the ABI instance and the shared module reference.  It
 * exposes the ordinary C++ plugin interfaces to PluginRuntime, but performs
 * no registration itself.  ABI callbacks are never allowed to throw across
 * this boundary and all plugin-provided output is copied into bounded host
 * storage before parsing.
 */
class DynamicPluginAdapter final : public IAlgorithmPlugin,
                                   public IManagedAlgorithmPlugin {
public:
    [[nodiscard]] static Result<std::shared_ptr<DynamicPluginAdapter>> create(
        std::shared_ptr<detail::DynamicModule> module,
        const iaisf_plugin_api& api,
        PluginMetadata metadata,
        nlohmann::json config = nlohmann::json::object());

    [[nodiscard]] static Result<std::shared_ptr<DynamicPluginAdapter>> create(
        std::shared_ptr<detail::DynamicModule> module,
        const iaisf_plugin_api& api,
        PluginMetadata metadata,
        PluginLimits limits,
        nlohmann::json config = nlohmann::json::object());

    ~DynamicPluginAdapter() noexcept override;

    DynamicPluginAdapter(const DynamicPluginAdapter&) = delete;
    DynamicPluginAdapter& operator=(const DynamicPluginAdapter&) = delete;
    DynamicPluginAdapter(DynamicPluginAdapter&&) = delete;
    DynamicPluginAdapter& operator=(DynamicPluginAdapter&&) = delete;

    [[nodiscard]] PluginMetadata metadata() const override;
    [[nodiscard]] Result<void> validate_input(
        const nlohmann::json& input) const override;
    [[nodiscard]] Result<nlohmann::json> execute(
        const nlohmann::json& input) const override;

    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;

    [[nodiscard]] DynamicPluginAdapterState state() const noexcept;

private:
    friend class PluginRuntime;

    /** Ends the module lifetime after shutdown/destroy have completed. */
    [[nodiscard]] bool release_module() noexcept;

    DynamicPluginAdapter(
        std::shared_ptr<detail::DynamicModule> module,
        iaisf_plugin_api api,
        PluginMetadata metadata,
        PluginLimits limits,
        std::string config_storage) noexcept;

    static void default_log(
        void* context,
        uint32_t level,
        iaisf_plugin_string_view message) noexcept;
    static void* default_allocate(void* context, size_t size) noexcept;
    static void default_deallocate(void* context, void* memory) noexcept;
    static iaisf_plugin_status_t collect_output(
        void* context,
        iaisf_plugin_bytes_view chunk) noexcept;

    [[nodiscard]] Result<iaisf_plugin_bytes_view> serialize_input(
        const nlohmann::json& input,
        std::string& storage) const;
    [[nodiscard]] Result<void> map_status(
        iaisf_plugin_status_t status,
        ErrorCode fallback,
        const char* operation) const;
    [[nodiscard]] bool destroy_instance_locked() noexcept;
    [[nodiscard]] Result<std::string> serialize_config(
        const nlohmann::json& config) const;

    std::shared_ptr<detail::DynamicModule> module_;
    iaisf_plugin_api api_{};
    PluginLimits limits_;
    PluginMetadata metadata_;
    iaisf_plugin_handle* instance_{nullptr};
    iaisf_plugin_host_api host_{};
    std::string config_storage_;
    mutable std::shared_mutex lifecycle_mutex_;
    DynamicPluginAdapterState state_{DynamicPluginAdapterState::Prepared};
    std::optional<Error> shutdown_error_;
    bool shutdown_called_{false};
    bool destroy_called_{false};
    bool initialize_supported_{false};
    bool initialized_{false};
};

}  // namespace iaisf::plugin
