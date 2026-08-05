#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "iaisf/plugin/abi/plugin_abi.h"
#include "iaisf/plugin/algorithm_plugin.hpp"
#include "iaisf/plugin/plugin_limits.hpp"

namespace iaisf::plugin::abi {

/**
 * Adapts an already supplied C ABI function table to the C++ plugin boundary.
 * No shared-library loading is performed here; the caller owns the function
 * table and must keep its code and context valid for this adapter's lifetime.
 */
class AbiPluginAdapter final : public IAlgorithmPlugin,
                               public IManagedAlgorithmPlugin {
public:
    [[nodiscard]] static Result<std::shared_ptr<AbiPluginAdapter>> create(
        iaisf_plugin_api api,
        PluginLimits limits);

    ~AbiPluginAdapter() noexcept override;

    AbiPluginAdapter(const AbiPluginAdapter&) = delete;
    AbiPluginAdapter& operator=(const AbiPluginAdapter&) = delete;
    AbiPluginAdapter(AbiPluginAdapter&&) = delete;
    AbiPluginAdapter& operator=(AbiPluginAdapter&&) = delete;

    [[nodiscard]] PluginMetadata metadata() const override;
    [[nodiscard]] Result<void> validate_input(
        const nlohmann::json& input) const override;
    [[nodiscard]] Result<nlohmann::json> execute(
        const nlohmann::json& input) const override;

    [[nodiscard]] Result<void> initialize() override;
    [[nodiscard]] Result<void> shutdown() override;

private:
    AbiPluginAdapter(
        iaisf_plugin_api api,
        PluginLimits limits,
        PluginMetadata metadata,
        iaisf_plugin_handle* instance) noexcept;

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

    iaisf_plugin_api api_{};
    PluginLimits limits_;
    PluginMetadata metadata_;
    iaisf_plugin_handle* instance_{nullptr};
    iaisf_plugin_host_api host_{};
    mutable std::shared_mutex lifecycle_mutex_;
    bool shutdown_called_{false};
};

}  // namespace iaisf::plugin::abi
