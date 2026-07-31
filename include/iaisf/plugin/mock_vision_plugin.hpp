#pragma once

#include "iaisf/plugin/algorithm_plugin.hpp"

namespace iaisf::plugin {

/**
 * Deterministic mock used only to exercise framework task flow.
 *
 * It does not read images or point clouds and does not run a model, GPU,
 * OpenCV, PCL, or TensorRT.
 */
class MockVisionPlugin final : public IAlgorithmPlugin {
public:
    [[nodiscard]] PluginMetadata metadata() const override;
    [[nodiscard]] Result<void> validate_input(
        const nlohmann::json& input) const override;
    [[nodiscard]] Result<nlohmann::json> execute(
        const nlohmann::json& input) const override;
};

}  // namespace iaisf::plugin
