#include "iaisf/application/application_contract.hpp"

#include <cstdint>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <utility>

#include "iaisf/api/strict_json.hpp"

namespace iaisf::application {
namespace {

using Json = nlohmann::json;
using ContractResult = ApplicationContractResult<ApplicationSubmitContract>;

// xyz-f32le is a wire format: three IEEE-754 binary32 coordinates, not a
// measurement of the host C++ float representation.
inline constexpr std::uint64_t kXyzF32LeBytesPerPoint = 12U;

ContractResult failure(
    const ApplicationContractErrorCategory category,
    const char* message) {
    return ContractResult::failure(ApplicationContractError{category, message});
}

ContractResult map_json_error(const api::StrictJsonError& error) {
    switch (error.category) {
        case api::StrictJsonFailureCategory::PayloadTooLarge:
            return failure(
                ApplicationContractErrorCategory::PayloadTooLarge,
                "request JSON exceeds the configured limit");
        case api::StrictJsonFailureCategory::ResourceFailure:
            return failure(
                ApplicationContractErrorCategory::ResourceFailure,
                "request JSON could not be processed");
        case api::StrictJsonFailureCategory::InternalFailure:
            return failure(
                ApplicationContractErrorCategory::InternalFailure,
                "request JSON processing failed");
        case api::StrictJsonFailureCategory::MalformedJson:
        case api::StrictJsonFailureCategory::DuplicateKey:
        case api::StrictJsonFailureCategory::DepthExceeded:
        case api::StrictJsonFailureCategory::NodeLimitExceeded:
        case api::StrictJsonFailureCategory::TextLimitExceeded:
            return failure(
                ApplicationContractErrorCategory::InvalidJson,
                "request is not valid strict JSON");
    }
    return failure(
        ApplicationContractErrorCategory::InternalFailure,
        "request JSON processing failed");
}

bool exact_keys(
    const Json& object,
    const std::set<std::string>& expected) {
    if (!object.is_object() || object.size() != expected.size()) {
        return false;
    }
    for (const auto& item : object.items()) {
        if (expected.find(item.key()) == expected.end()) {
            return false;
        }
    }
    return true;
}

bool coordinate_frame_token(const std::string& value) noexcept {
    if (value.empty() || value.size() > kMaxCoordinateFrameBytes ||
        value == "." || value == "..") {
        return false;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool alnum =
            (byte >= static_cast<unsigned char>('0') &&
             byte <= static_cast<unsigned char>('9')) ||
            (byte >= static_cast<unsigned char>('A') &&
             byte <= static_cast<unsigned char>('Z')) ||
            (byte >= static_cast<unsigned char>('a') &&
             byte <= static_cast<unsigned char>('z'));
        if (!alnum && character != '_' && character != '-' &&
            character != '.') {
            return false;
        }
    }
    return true;
}

bool exact_string(
    const Json& object,
    const char* key,
    std::string& output) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_string()) {
        return false;
    }
    output = iterator->get<std::string>();
    return true;
}

bool exact_unsigned(
    const Json& object,
    const char* key,
    std::uint64_t& output) {
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_number_unsigned()) {
        return false;
    }
    output = iterator->get<std::uint64_t>();
    return true;
}

ApplicationContractResult<ArtifactRef> parse_artifact(const Json& object) {
    if (!exact_keys(
            object,
            {"artifact_id", "sha256", "size_bytes", "kind",
             "media_type", "coordinate_frame", "unit", "point_count"})) {
        return ApplicationContractResult<ArtifactRef>::failure({
            ApplicationContractErrorCategory::InvalidRequest,
            "input artifact schema is invalid"});
    }

    ArtifactRef artifact;
    if (!exact_string(object, "artifact_id", artifact.artifact_id) ||
        !exact_string(object, "sha256", artifact.sha256) ||
        !exact_string(object, "kind", artifact.kind) ||
        !exact_string(object, "media_type", artifact.media_type) ||
        !exact_string(
            object, "coordinate_frame", artifact.coordinate_frame.emplace()) ||
        !exact_string(object, "unit", artifact.unit.emplace()) ||
        !exact_unsigned(object, "size_bytes", artifact.size_bytes)) {
        return ApplicationContractResult<ArtifactRef>::failure({
            ApplicationContractErrorCategory::InvalidRequest,
            "input artifact field type is invalid"});
    }
    std::uint64_t point_count = 0U;
    if (!exact_unsigned(object, "point_count", point_count)) {
        return ApplicationContractResult<ArtifactRef>::failure({
            ApplicationContractErrorCategory::InvalidRequest,
            "input artifact field type is invalid"});
    }
    artifact.point_count = point_count;

    if (artifact.kind != "point_cloud" ||
        artifact.media_type != "application/vnd.iaisf.pointcloud.xyz-f32le" ||
        artifact.unit != std::optional<std::string>{"mm"} ||
        !coordinate_frame_token(*artifact.coordinate_frame)) {
        return ApplicationContractResult<ArtifactRef>::failure({
            ApplicationContractErrorCategory::ValidationFailed,
            "input artifact metadata is not supported"});
    }
    if (point_count == 0U ||
        point_count > std::numeric_limits<std::uint64_t>::max() /
                           kXyzF32LeBytesPerPoint ||
        point_count * kXyzF32LeBytesPerPoint != artifact.size_bytes) {
        return ApplicationContractResult<ArtifactRef>::failure({
            ApplicationContractErrorCategory::ValidationFailed,
            "input artifact size and point count are inconsistent"});
    }
    const auto valid = validate_artifact_ref(artifact);
    if (!valid) {
        return ApplicationContractResult<ArtifactRef>::failure({
            ApplicationContractErrorCategory::ValidationFailed,
            "input artifact value is invalid"});
    }
    return ApplicationContractResult<ArtifactRef>::success(std::move(artifact));
}

ContractResult parse_root(
    const std::string_view body,
    const IndustrialApplication application,
    const ScenePhase scene_phase,
    const bool inspection) {
    const auto parsed = api::parse_strict_json(body);
    if (!parsed) {
        return map_json_error(parsed.error());
    }
    const auto& root = parsed.value();
    const std::set<std::string> expected = inspection
                                               ? std::set<std::string>{
                                                     "schema_version",
                                                     "input_artifacts",
                                                     "requested_outputs"}
                                               : std::set<std::string>{
                                                     "schema_version",
                                                     "input_artifacts",
                                                     "weld_type",
                                                     "review_policy"};
    if (!exact_keys(root, expected)) {
        return failure(
            ApplicationContractErrorCategory::InvalidRequest,
            "request object contains unknown or missing fields");
    }
    std::string schema_version;
    if (!exact_string(root, "schema_version", schema_version) ||
        schema_version != "1.0") {
        return failure(
            ApplicationContractErrorCategory::InvalidRequest,
            "schema_version must be 1.0");
    }
    const auto artifacts_iterator = root.find("input_artifacts");
    if (artifacts_iterator == root.end() ||
        !artifacts_iterator->is_array() || artifacts_iterator->size() != 1U) {
        return failure(
            ApplicationContractErrorCategory::InvalidRequest,
            "exactly one input artifact is required");
    }
    auto artifact = parse_artifact((*artifacts_iterator)[0]);
    if (!artifact) {
        return failure(artifact.error().category, artifact.error().message.c_str());
    }

    if (inspection) {
        const auto outputs_iterator = root.find("requested_outputs");
        bool segmentation = false;
        bool geometry = false;
        if (outputs_iterator == root.end() || !outputs_iterator->is_array() ||
            outputs_iterator->empty() || outputs_iterator->size() > 2U) {
            return failure(
                ApplicationContractErrorCategory::InvalidRequest,
                "requested_outputs is invalid");
        }
        for (const auto& item : *outputs_iterator) {
            if (!item.is_string()) {
                return failure(
                    ApplicationContractErrorCategory::InvalidRequest,
                    "requested_outputs contains an invalid value");
            }
            const auto output = item.get<std::string>();
            if (output == "segmentation" && !segmentation) {
                segmentation = true;
            } else if (output == "geometry" && !geometry) {
                geometry = true;
            } else {
                return failure(
                    ApplicationContractErrorCategory::InvalidRequest,
                    "requested_outputs contains an unknown or duplicate value");
            }
        }
        const auto outputs = InspectionRequestedOutputs::create(
            segmentation, geometry);
        if (!outputs) {
            return failure(
                ApplicationContractErrorCategory::ValidationFailed,
                "inspection outputs are invalid");
        }
        const auto inspection_submission =
            WeldInspectionSubmission::create(outputs.value());
        const auto spec = ApplicationSubmissionSpec::create(
            inspection_submission.value());
        return ContractResult::success(ApplicationSubmitContract{
            application, scene_phase, std::move(spec).value(),
            std::move(artifact).value()});
    }

    const auto weld_type = root.find("weld_type");
    const auto review = root.find("review_policy");
    std::string mode;
    if (!weld_type->is_object() || !review->is_object() ||
        !exact_string(*weld_type, "mode", mode)) {
        return failure(
            ApplicationContractErrorCategory::InvalidRequest,
            "weld_type is invalid");
    }
    std::optional<RequestedWeldType> requested;
    if (mode == "auto") {
        if (!exact_keys(*weld_type, {"mode"})) {
            return failure(
                ApplicationContractErrorCategory::InvalidRequest,
                "auto weld_type must contain only mode");
        }
    } else if (mode == "requested") {
        std::string requested_text;
        if (!exact_keys(*weld_type, {"mode", "requested"}) ||
            !exact_string(*weld_type, "requested", requested_text)) {
            return failure(
                ApplicationContractErrorCategory::InvalidRequest,
                "requested weld_type is invalid");
        }
        if (requested_text == "straight") {
            requested = RequestedWeldType::Straight;
        } else if (requested_text == "corner") {
            requested = RequestedWeldType::Corner;
        } else if (requested_text == "l") {
            requested = RequestedWeldType::L;
        } else {
            return failure(
                ApplicationContractErrorCategory::ValidationFailed,
                "requested weld type is not supported");
        }
    } else {
        return failure(
            ApplicationContractErrorCategory::ValidationFailed,
            "weld type mode is not supported");
    }
    std::string checkpoint;
    if (!exact_keys(*review, {"human_checkpoint"}) ||
        !exact_string(*review, "human_checkpoint", checkpoint)) {
        return failure(
            ApplicationContractErrorCategory::InvalidRequest,
            "human checkpoint policy is invalid");
    }
    if (checkpoint != "required") {
        return failure(
            ApplicationContractErrorCategory::ValidationFailed,
            "human checkpoint policy is not supported");
    }
    const auto weld_request = WeldTypeRequest::create(
        mode == "auto" ? WeldTypeMode::Auto : WeldTypeMode::Requested,
        requested);
    if (!weld_request) {
        return failure(
            ApplicationContractErrorCategory::ValidationFailed,
            "weld type request is invalid");
    }
    const auto guidance = WeldingGuidanceSubmission::create(
        weld_request.value(), HumanCheckpointPolicy::Required);
    if (!guidance) {
        return failure(
            ApplicationContractErrorCategory::ValidationFailed,
            "guidance submission is invalid");
    }
    const auto spec = ApplicationSubmissionSpec::create(guidance.value());
    if (!spec) {
        return failure(
            ApplicationContractErrorCategory::ValidationFailed,
            "guidance submission is invalid");
    }
    return ContractResult::success(ApplicationSubmitContract{
        application, scene_phase, std::move(spec).value(),
        std::move(artifact).value()});
}

}  // namespace

std::string_view to_string(
    const ApplicationContractErrorCategory category) noexcept {
    switch (category) {
        case ApplicationContractErrorCategory::InvalidJson:
            return "invalid_json";
        case ApplicationContractErrorCategory::InvalidRequest:
            return "invalid_request";
        case ApplicationContractErrorCategory::ValidationFailed:
            return "validation_failed";
        case ApplicationContractErrorCategory::PayloadTooLarge:
            return "payload_too_large";
        case ApplicationContractErrorCategory::ResourceFailure:
            return "resource_failure";
        case ApplicationContractErrorCategory::InternalFailure:
            return "internal_failure";
    }
    return "internal_failure";
}

ApplicationContractResult<ApplicationSubmitContract>
parse_weld_inspection_submit(const std::string_view body) {
    try {
        return parse_root(
            body,
            IndustrialApplication::WeldInspection,
            ScenePhase::PostWeld,
            true);
    } catch (const std::bad_alloc&) {
        return failure(
            ApplicationContractErrorCategory::ResourceFailure,
            "unable to allocate application contract");
    } catch (const std::exception&) {
        return failure(
            ApplicationContractErrorCategory::InternalFailure,
            "application contract processing failed");
    }
}

ApplicationContractResult<ApplicationSubmitContract>
parse_welding_guidance_submit(const std::string_view body) {
    try {
        return parse_root(
            body,
            IndustrialApplication::WeldingGuidance,
            ScenePhase::PreWeld,
            false);
    } catch (const std::bad_alloc&) {
        return failure(
            ApplicationContractErrorCategory::ResourceFailure,
            "unable to allocate application contract");
    } catch (const std::exception&) {
        return failure(
            ApplicationContractErrorCategory::InternalFailure,
            "application contract processing failed");
    }
}

}  // namespace iaisf::application
