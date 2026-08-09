#include "iaisf/application/application_submission.hpp"

#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] Result<void> invalid(const char* message) {
    return Result<void>::failure(make_error(ErrorCode::InvalidArgument, message));
}

[[nodiscard]] bool valid_mode(const WeldTypeMode mode) noexcept {
    return mode == WeldTypeMode::Auto || mode == WeldTypeMode::Requested;
}

[[nodiscard]] bool valid_requested_type(
    const RequestedWeldType type) noexcept {
    switch (type) {
        case RequestedWeldType::Straight:
        case RequestedWeldType::Corner:
        case RequestedWeldType::L:
            return true;
    }
    return false;
}

[[nodiscard]] bool valid_policy(
    const HumanCheckpointPolicy policy) noexcept {
    return policy == HumanCheckpointPolicy::Required ||
           policy == HumanCheckpointPolicy::NotRequired;
}

}  // namespace

std::string_view to_string(
    const ApplicationSubmissionKind kind) noexcept {
    switch (kind) {
        case ApplicationSubmissionKind::WeldInspection:
            return "weld_inspection";
        case ApplicationSubmissionKind::WeldingGuidance:
            return "welding_guidance";
    }
    return "unknown";
}

std::string_view to_string(const WeldTypeMode mode) noexcept {
    switch (mode) {
        case WeldTypeMode::Auto:
            return "auto";
        case WeldTypeMode::Requested:
            return "requested";
    }
    return "unknown";
}

std::string_view to_string(const RequestedWeldType type) noexcept {
    switch (type) {
        case RequestedWeldType::Straight:
            return "straight";
        case RequestedWeldType::Corner:
            return "corner";
        case RequestedWeldType::L:
            return "l";
    }
    return "unknown";
}

std::string_view to_string(
    const HumanCheckpointPolicy policy) noexcept {
    switch (policy) {
        case HumanCheckpointPolicy::Required:
            return "required";
        case HumanCheckpointPolicy::NotRequired:
            return "not_required";
    }
    return "unknown";
}

Result<InspectionRequestedOutputs> InspectionRequestedOutputs::create(
    const bool segmentation,
    const bool geometry) {
    if (!segmentation && !geometry) {
        return Result<InspectionRequestedOutputs>::failure(make_error(
            ErrorCode::InvalidArgument,
            "at least one inspection output is required"));
    }
    const auto mask = static_cast<std::uint8_t>(
        (segmentation ? 0x01U : 0U) | (geometry ? 0x02U : 0U));
    return Result<InspectionRequestedOutputs>::success(
        InspectionRequestedOutputs{mask});
}

bool InspectionRequestedOutputs::requests_segmentation() const noexcept {
    return (mask_ & 0x01U) != 0U;
}

bool InspectionRequestedOutputs::requests_geometry() const noexcept {
    return (mask_ & 0x02U) != 0U;
}

Result<WeldInspectionSubmission> WeldInspectionSubmission::create(
    const InspectionRequestedOutputs outputs) {
    if (!outputs.requests_segmentation() && !outputs.requests_geometry()) {
        return Result<WeldInspectionSubmission>::failure(make_error(
            ErrorCode::InvalidArgument,
            "inspection submission must request an output"));
    }
    return Result<WeldInspectionSubmission>::success(
        WeldInspectionSubmission{outputs});
}

const InspectionRequestedOutputs&
WeldInspectionSubmission::outputs() const noexcept {
    return outputs_;
}

Result<WeldTypeRequest> WeldTypeRequest::create(
    const WeldTypeMode mode,
    const std::optional<RequestedWeldType> requested_type) {
    if (!valid_mode(mode)) {
        return Result<WeldTypeRequest>::failure(make_error(
            ErrorCode::InvalidArgument,
            "weld type mode is invalid"));
    }
    if (mode == WeldTypeMode::Auto && requested_type.has_value()) {
        return Result<WeldTypeRequest>::failure(make_error(
            ErrorCode::InvalidArgument,
            "auto weld type must not include a requested type"));
    }
    if (mode == WeldTypeMode::Requested &&
        (!requested_type.has_value() ||
         !valid_requested_type(*requested_type))) {
        return Result<WeldTypeRequest>::failure(make_error(
            ErrorCode::InvalidArgument,
            "requested weld type is required and must be valid"));
    }
    return Result<WeldTypeRequest>::success(
        WeldTypeRequest{mode, requested_type});
}

WeldTypeMode WeldTypeRequest::mode() const noexcept {
    return mode_;
}

std::optional<RequestedWeldType> WeldTypeRequest::requested_type()
    const noexcept {
    return requested_type_;
}

Result<WeldingGuidanceSubmission> WeldingGuidanceSubmission::create(
    const WeldTypeRequest weld_type,
    const HumanCheckpointPolicy human_checkpoint) {
    if (!valid_policy(human_checkpoint)) {
        return Result<WeldingGuidanceSubmission>::failure(make_error(
            ErrorCode::InvalidArgument,
            "human checkpoint policy is invalid"));
    }
    return Result<WeldingGuidanceSubmission>::success(
        WeldingGuidanceSubmission{weld_type, human_checkpoint});
}

const WeldTypeRequest& WeldingGuidanceSubmission::weld_type() const noexcept {
    return weld_type_;
}

HumanCheckpointPolicy
WeldingGuidanceSubmission::human_checkpoint() const noexcept {
    return human_checkpoint_;
}

Result<ApplicationSubmissionSpec> ApplicationSubmissionSpec::create(
    WeldInspectionSubmission inspection) {
    return Result<ApplicationSubmissionSpec>::success(
        ApplicationSubmissionSpec{Value{std::move(inspection)}});
}

Result<ApplicationSubmissionSpec> ApplicationSubmissionSpec::create(
    WeldingGuidanceSubmission guidance) {
    return Result<ApplicationSubmissionSpec>::success(
        ApplicationSubmissionSpec{Value{std::move(guidance)}});
}

ApplicationSubmissionKind ApplicationSubmissionSpec::kind() const noexcept {
    return std::holds_alternative<WeldInspectionSubmission>(value_)
               ? ApplicationSubmissionKind::WeldInspection
               : ApplicationSubmissionKind::WeldingGuidance;
}

const WeldInspectionSubmission*
ApplicationSubmissionSpec::inspection() const noexcept {
    return std::get_if<WeldInspectionSubmission>(&value_);
}

const WeldingGuidanceSubmission*
ApplicationSubmissionSpec::guidance() const noexcept {
    return std::get_if<WeldingGuidanceSubmission>(&value_);
}

Result<void> ApplicationSubmissionSpec::validate_for(
    const IndustrialApplication application,
    const ScenePhase scene_phase) const {
    const auto identity = validate_application_scene(application, scene_phase);
    if (!identity) {
        return identity;
    }
    if (application == IndustrialApplication::WeldInspection &&
        inspection() == nullptr) {
        return invalid("inspection application requires inspection submission");
    }
    if (application == IndustrialApplication::WeldingGuidance &&
        guidance() == nullptr) {
        return invalid("guidance application requires guidance submission");
    }
    return Result<void>::success();
}

}  // namespace iaisf::application
