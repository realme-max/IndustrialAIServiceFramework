#pragma once

#include <memory>

#include "iaisf/core/result.hpp"
#include "iaisf/plugin/plugin_manager.hpp"
#include "iaisf/task/task_types.hpp"

namespace iaisf::plugin {

/**
 * Adapts a frozen PluginManager to TaskManager validator/handler functions.
 *
 * Closures returned by make_validator() and make_handler() capture only a
 * shared_ptr<const PluginManager>. They do not retain the adapter or a
 * TaskManager, so no ownership cycle is formed. Task admission performs
 * ordinary user-input validation. The worker performs a defensive second
 * plugin validation; a changed outcome is treated as an internal
 * plugin-contract failure and execute() is not called.
 */
class PluginTaskAdapter {
    struct ConstructionKey {
        explicit ConstructionKey() = default;
    };

public:
    [[nodiscard]] static Result<std::shared_ptr<PluginTaskAdapter>> create(
        std::shared_ptr<PluginManager> manager);

    PluginTaskAdapter(const PluginTaskAdapter&) = delete;
    PluginTaskAdapter& operator=(const PluginTaskAdapter&) = delete;
    PluginTaskAdapter(PluginTaskAdapter&&) = delete;
    PluginTaskAdapter& operator=(PluginTaskAdapter&&) = delete;

    PluginTaskAdapter(
        ConstructionKey,
        std::shared_ptr<PluginManager> manager);

    [[nodiscard]] Result<void> validate_task(
        const task::TaskRequest& request) const;
    [[nodiscard]] Result<nlohmann::json> execute_task(
        const task::TaskRequest& request) const;

    [[nodiscard]] task::TaskValidator make_validator() const;
    [[nodiscard]] task::TaskHandler make_handler() const;

private:
    std::shared_ptr<const PluginManager> manager_;
};

}  // namespace iaisf::plugin
