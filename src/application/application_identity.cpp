#include "iaisf/application/application_identity.hpp"

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] bool valid_application(
    const IndustrialApplication application) noexcept {
    switch (application) {
        case IndustrialApplication::WeldInspection:
        case IndustrialApplication::WeldingGuidance:
            return true;
    }
    return false;
}

[[nodiscard]] bool valid_phase(const ScenePhase phase) noexcept {
    switch (phase) {
        case ScenePhase::PostWeld:
        case ScenePhase::PreWeld:
            return true;
    }
    return false;
}

}  // namespace

std::string_view to_string(const IndustrialApplication application) noexcept {
    switch (application) {
        case IndustrialApplication::WeldInspection:
            return "weld_inspection";
        case IndustrialApplication::WeldingGuidance:
            return "welding_guidance";
    }
    return "unknown";
}

std::string_view to_string(const ScenePhase phase) noexcept {
    switch (phase) {
        case ScenePhase::PostWeld:
            return "post_weld";
        case ScenePhase::PreWeld:
            return "pre_weld";
    }
    return "unknown";
}

Result<IndustrialApplication> parse_industrial_application(
    const std::string_view value) {
    if (value == "weld_inspection") {
        return Result<IndustrialApplication>::success(
            IndustrialApplication::WeldInspection);
    }
    if (value == "welding_guidance") {
        return Result<IndustrialApplication>::success(
            IndustrialApplication::WeldingGuidance);
    }
    return Result<IndustrialApplication>::failure(make_error(
        ErrorCode::InvalidArgument,
        "industrial application is not supported"));
}

Result<ScenePhase> parse_scene_phase(const std::string_view value) {
    if (value == "post_weld") {
        return Result<ScenePhase>::success(ScenePhase::PostWeld);
    }
    if (value == "pre_weld") {
        return Result<ScenePhase>::success(ScenePhase::PreWeld);
    }
    return Result<ScenePhase>::failure(make_error(
        ErrorCode::InvalidArgument,
        "scene phase is not supported"));
}

Result<void> validate_application_scene(
    const IndustrialApplication application,
    const ScenePhase phase) {
    if (!valid_application(application) || !valid_phase(phase)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application or scene phase is invalid"));
    }
    const bool valid_pair =
        (application == IndustrialApplication::WeldInspection &&
         phase == ScenePhase::PostWeld) ||
        (application == IndustrialApplication::WeldingGuidance &&
         phase == ScenePhase::PreWeld);
    if (!valid_pair) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application and scene phase do not match"));
    }
    return Result<void>::success();
}

}  // namespace iaisf::application
