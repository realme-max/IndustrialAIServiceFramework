#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "iaisf/core/result.hpp"
#include "iaisf/plugin/algorithm_plugin.hpp"
#include "iaisf/plugin/plugin_limits.hpp"
#include "iaisf/plugin/plugin_metadata.hpp"

namespace iaisf::plugin {

class PluginTaskAdapter;

/**
 * Explicit in-process plugin registry.
 *
 * Registration is allowed only while Configuring; lookup, list, validation,
 * and execution are rejected in that state. freeze() is idempotent and
 * permanently makes the registry read-only. A frozen manager rejects
 * registration and supports concurrent lookup, validation, and execution.
 * Registry mutexes are released before plugin code is invoked. Input and
 * output JSON pass the configured structural and byte limits; execute() also
 * performs plugin validation for direct callers.
 */
class PluginManager {
public:
    explicit PluginManager(PluginLimits limits);

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

    [[nodiscard]] Result<void> register_plugin(
        std::shared_ptr<const IAlgorithmPlugin> plugin);
    [[nodiscard]] Result<void> freeze();
    [[nodiscard]] bool frozen() const;
    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] Result<PluginMetadata> find_metadata(
        std::string_view operation) const;
    [[nodiscard]] Result<std::vector<PluginMetadata>> list_metadata() const;
    [[nodiscard]] Result<void> validate(
        std::string_view operation,
        const nlohmann::json& input) const;
    [[nodiscard]] Result<nlohmann::json> execute(
        std::string_view operation,
        const nlohmann::json& input) const;

    [[nodiscard]] const PluginLimits& limits() const noexcept;

private:
    friend class PluginTaskAdapter;

    struct Entry {
        PluginMetadata metadata;
        std::shared_ptr<const IAlgorithmPlugin> plugin;
    };

    [[nodiscard]] Result<std::shared_ptr<const IAlgorithmPlugin>> find_plugin(
        std::string_view operation) const;
    [[nodiscard]] Result<nlohmann::json> execute_validated(
        std::string_view operation,
        const nlohmann::json& input) const;
    [[nodiscard]] Result<void> validate_plugin_contract(
        std::string_view operation,
        const nlohmann::json& input) const;
    [[nodiscard]] Result<void> invoke_plugin_validation(
        const std::shared_ptr<const IAlgorithmPlugin>& plugin,
        const nlohmann::json& input) const;
    [[nodiscard]] Result<void> validate_input_capacity(
        const nlohmann::json& input) const;
    [[nodiscard]] Result<void> validate_output_capacity(
        const nlohmann::json& output) const;
    [[nodiscard]] Error normalized_plugin_error(
        const Error& error,
        bool validation) const;
    [[nodiscard]] Error fixed_internal_error(
        std::string_view message) const;
    [[nodiscard]] Error bounded_error(
        ErrorCode code,
        std::string_view message) const;

    PluginLimits limits_;
    mutable std::mutex mutex_;
    bool frozen_{false};
    std::unordered_map<std::string, Entry> registry_;
};

}  // namespace iaisf::plugin
