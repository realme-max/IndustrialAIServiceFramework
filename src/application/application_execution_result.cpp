#include "iaisf/application/application_execution_result.hpp"

#include <cmath>
#include <limits>
#include <string_view>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

constexpr std::size_t kMaxOutputArtifacts = 16U;
constexpr std::size_t kMaxResultCoordinateFrameBytes = 128U;
constexpr std::size_t kMaxReasonBytes = 256U;

Result<void> invalid(const char* message) {
    return Result<void>::failure(make_error(ErrorCode::InvalidArgument, message));
}

bool finite_point(const ApplicationPoint3& point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y) &&
           std::isfinite(point.z);
}

bool bounded_text(const std::string& value, const std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte == 0x7fU) {
            return false;
        }
    }
    return true;
}

bool valid_requested_type(const RequestedWeldType type) noexcept {
    switch (type) {
        case RequestedWeldType::Straight:
        case RequestedWeldType::Corner:
        case RequestedWeldType::L:
            return true;
    }
    return false;
}

Result<void> validate_outputs(const std::vector<ArtifactRef>& artifacts) {
    if (artifacts.empty() || artifacts.size() > kMaxOutputArtifacts) {
        return invalid("application result output artifacts are invalid");
    }
    for (std::size_t index = 0U; index < artifacts.size(); ++index) {
        const auto valid = validate_artifact_ref(artifacts[index]);
        if (!valid) {
            return invalid("application result contains an invalid artifact");
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (artifacts[previous].artifact_id == artifacts[index].artifact_id) {
                return invalid("application result contains duplicate artifacts");
            }
        }
    }
    return Result<void>::success();
}

Result<void> validate_optional_artifact(const std::optional<ArtifactRef>& artifact) {
    if (!artifact.has_value()) {
        return Result<void>::success();
    }
    return validate_artifact_ref(*artifact);
}

}  // namespace

std::string_view to_string(
    const GuidanceResultDisposition disposition) noexcept {
    switch (disposition) {
        case GuidanceResultDisposition::Completed:
            return "completed";
        case GuidanceResultDisposition::WaitingHuman:
            return "waiting-human";
    }
    return "unknown";
}

Result<void> validate_execution_result(
    const ApplicationExecutionResult& result,
    const IndustrialApplication application) {
    if (application == IndustrialApplication::WeldInspection) {
        const auto* inspection = std::get_if<WeldInspectionResult>(&result);
        if (inspection == nullptr) {
            return invalid("inspection result does not match application");
        }
        if (!validate_outputs(inspection->output_artifacts) ||
            !validate_optional_artifact(inspection->weld_points) ||
            !validate_optional_artifact(inspection->prediction) ||
            inspection->quality_assessment != "not_implemented" ||
            !std::isfinite(inspection->weld_ratio) ||
            inspection->weld_ratio < 0.0 || inspection->weld_ratio > 1.0 ||
            !std::isfinite(inspection->length_mm) ||
            !std::isfinite(inspection->inference_time_ms) ||
            !std::isfinite(inspection->total_time_ms) ||
            inspection->length_mm < 0.0 || inspection->inference_time_ms < 0.0 ||
            inspection->total_time_ms < 0.0 ||
            inspection->quality_assessment.size() > kMaxReasonBytes) {
            return invalid("inspection result is invalid");
        }
        return Result<void>::success();
    }

    if (application != IndustrialApplication::WeldingGuidance) {
        return invalid("application is invalid for execution result");
    }
    const auto* guidance = std::get_if<WeldingGuidanceResult>(&result);
    if (guidance == nullptr || !validate_outputs(guidance->output_artifacts) ||
        !bounded_text(guidance->coordinate_frame, kMaxResultCoordinateFrameBytes) ||
        guidance->unit != "mm" || !valid_requested_type(guidance->weld_type)) {
        return invalid("guidance result is invalid");
    }
    if (guidance->confidence.has_value() &&
        (!std::isfinite(*guidance->confidence) || *guidance->confidence < 0.0 ||
         *guidance->confidence > 1.0)) {
        return invalid("guidance confidence is invalid");
    }
    if ((guidance->x_axis.has_value() && !finite_point(*guidance->x_axis)) ||
        (guidance->y_axis.has_value() && !finite_point(*guidance->y_axis)) ||
        (guidance->z_axis.has_value() && !finite_point(*guidance->z_axis))) {
        return invalid("guidance axes are invalid");
    }
    if (guidance->disposition == GuidanceResultDisposition::WaitingHuman) {
        if (!guidance->waiting_reason.has_value() ||
            !bounded_text(*guidance->waiting_reason, kMaxReasonBytes) ||
            guidance->robot_execution_allowed || guidance->corner.has_value()) {
            return invalid("waiting-human guidance result is invalid");
        }
        return Result<void>::success();
    }
    if (guidance->disposition != GuidanceResultDisposition::Completed ||
        guidance->waiting_reason.has_value() || guidance->robot_execution_allowed ||
        !guidance->start.has_value() || !guidance->end.has_value() ||
        !finite_point(*guidance->start) || !finite_point(*guidance->end)) {
        return invalid("completed guidance result is invalid");
    }
    if (guidance->weld_type == RequestedWeldType::L) {
        if (!guidance->corner.has_value() || !finite_point(*guidance->corner)) {
            return invalid("L guidance result requires a corner point");
        }
    } else if (guidance->corner.has_value()) {
        return invalid("straight or corner guidance must not include a corner");
    }
    return Result<void>::success();
}

}  // namespace iaisf::application
