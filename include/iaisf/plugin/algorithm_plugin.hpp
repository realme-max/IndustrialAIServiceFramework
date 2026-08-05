#pragma once

#include <nlohmann/json.hpp>

#include "iaisf/core/result.hpp"
#include "iaisf/plugin/plugin_metadata.hpp"

namespace iaisf::plugin {

/**
 * Minimal, cross-platform algorithm boundary.
 *
 * validate_input() must be fast, deterministic, repeatable, free of I/O and
 * free of externally visible side effects. validate_input() and execute() may
 * run concurrently on the same instance, and execute() may run concurrently
 * for unrelated tasks. Implementations therefore own all synchronization and
 * must not rely on const methods being inherently thread-safe. Neither method
 * receives network, EventLoop, or repository objects.
 */
class IAlgorithmPlugin {
public:
    virtual ~IAlgorithmPlugin() = default;

    [[nodiscard]] virtual PluginMetadata metadata() const = 0;
    [[nodiscard]] virtual Result<void> validate_input(
        const nlohmann::json& input) const = 0;
    [[nodiscard]] virtual Result<nlohmann::json> execute(
        const nlohmann::json& input) const = 0;
};

/**
 * Optional lifecycle capability for in-process plugins.
 *
 * The existing algorithm contract remains valid for stateless plugins.  A
 * managed plugin may opt in to composition-root initialization and shutdown.  A
 * lifecycle implementation must not throw across the runtime boundary; the
 * runtime converts any violation into a stable Error.
 */
class IManagedAlgorithmPlugin {
public:
    virtual ~IManagedAlgorithmPlugin() = default;

    [[nodiscard]] virtual Result<void> initialize() = 0;
    [[nodiscard]] virtual Result<void> shutdown() = 0;
};

}  // namespace iaisf::plugin
