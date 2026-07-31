#pragma once

#include "iaisf/plugin/algorithm_plugin.hpp"

namespace iaisf::plugin {

/**
 * Stateless echo operation.
 *
 * Input must contain exactly one "payload" member. Successful execution
 * returns an independent copy of that member without an envelope.
 */
class EchoPlugin final : public IAlgorithmPlugin {
public:
    [[nodiscard]] PluginMetadata metadata() const override;
    [[nodiscard]] Result<void> validate_input(
        const nlohmann::json& input) const override;
    [[nodiscard]] Result<nlohmann::json> execute(
        const nlohmann::json& input) const override;
};

}  // namespace iaisf::plugin
