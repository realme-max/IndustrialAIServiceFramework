#pragma once

#include <string_view>

#include "iaisf/core/result.hpp"

namespace iaisf::application {

enum class IndustrialApplication {
    WeldInspection,
    WeldingGuidance,
};

enum class ScenePhase {
    PostWeld,
    PreWeld,
};

[[nodiscard]] std::string_view to_string(
    IndustrialApplication application) noexcept;
[[nodiscard]] std::string_view to_string(ScenePhase phase) noexcept;

[[nodiscard]] Result<IndustrialApplication> parse_industrial_application(
    std::string_view value);
[[nodiscard]] Result<ScenePhase> parse_scene_phase(
    std::string_view value);

/** Validates the only two supported application/scene pairings. */
[[nodiscard]] Result<void> validate_application_scene(
    IndustrialApplication application,
    ScenePhase phase);

}  // namespace iaisf::application
