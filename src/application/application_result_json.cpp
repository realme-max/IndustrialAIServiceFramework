#include "iaisf/application/application_result_json.hpp"

#include <new>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

using Json = nlohmann::ordered_json;

Json artifact_json(const ArtifactRef& artifact,
                   const std::string_view download_prefix) {
    Json output{
        {"artifact_id", artifact.artifact_id},
        {"sha256", artifact.sha256},
        {"size_bytes", artifact.size_bytes},
        {"kind", artifact.kind},
        {"media_type", artifact.media_type}};
    output["download_url"] =
        std::string(download_prefix) + artifact.artifact_id;
    if (artifact.coordinate_frame.has_value()) {
        output["coordinate_frame"] = *artifact.coordinate_frame;
    }
    if (artifact.unit.has_value()) {
        output["unit"] = *artifact.unit;
    }
    if (artifact.point_count.has_value()) {
        output["point_count"] = *artifact.point_count;
    }
    return output;
}

Json point_json(const ApplicationPoint3& point) {
    return Json::array({point.x, point.y, point.z});
}

Result<std::string> failure(const ErrorCode code, const char* message) {
    return Result<std::string>::failure(make_error(code, message));
}

}  // namespace

Result<std::string> application_execution_result_json(
    const ApplicationJobSnapshot& snapshot,
    const std::size_t maximum_bytes,
    const std::string_view artifact_download_prefix) {
    try {
        if (maximum_bytes == 0U || maximum_bytes > kMaxApplicationResultBodyBytes ||
            !snapshot.job_id().valid() || snapshot.execution_result() == nullptr ||
            to_string(snapshot.state()) == "unknown") {
            return failure(ErrorCode::InvalidArgument,
                           "application execution result is unavailable");
        }
        const auto valid = validate_execution_result(
            *snapshot.execution_result(), snapshot.application());
        if (!valid) {
            return failure(ErrorCode::InvalidArgument,
                           "application execution result is invalid");
        }
        Json body{
            {"schema_version", "1.0"},
            {"job_id", std::string(snapshot.job_id().value())},
            {"application", to_string(snapshot.application())},
            {"state", to_string(snapshot.state())},
            {"version", snapshot.version()}};
        Json outputs = Json::array();
        if (const auto* inspection = std::get_if<WeldInspectionResult>(
                snapshot.execution_result())) {
            for (const auto& artifact : inspection->output_artifacts) {
                outputs.push_back(artifact_json(artifact, artifact_download_prefix));
            }
            body["output_artifacts"] = std::move(outputs);
            if (inspection->weld_points.has_value()) {
                body["weld_points"] = artifact_json(
                    *inspection->weld_points, artifact_download_prefix);
            }
            if (inspection->prediction.has_value()) {
                body["prediction"] = artifact_json(
                    *inspection->prediction, artifact_download_prefix);
            }
            body["weld_point_count"] = inspection->weld_point_count;
            body["weld_ratio"] = inspection->weld_ratio;
            body["length_mm"] = inspection->length_mm;
            body["inference_time_ms"] = inspection->inference_time_ms;
            body["total_time_ms"] = inspection->total_time_ms;
            body["quality_assessment"] = "not_implemented";
        } else {
            const auto& guidance = std::get<WeldingGuidanceResult>(
                *snapshot.execution_result());
            for (const auto& artifact : guidance.output_artifacts) {
                outputs.push_back(artifact_json(artifact, artifact_download_prefix));
            }
            body["output_artifacts"] = std::move(outputs);
            body["weld_type"] = to_string(guidance.weld_type);
            body["coordinate_frame"] = guidance.coordinate_frame;
            body["unit"] = guidance.unit;
            body["disposition"] = to_string(guidance.disposition);
            body["robot_execution_allowed"] = false;
            if (guidance.start.has_value()) {
                body["start"] = point_json(*guidance.start);
            }
            if (guidance.end.has_value()) {
                body["end"] = point_json(*guidance.end);
            }
            if (guidance.corner.has_value()) {
                body["corner"] = point_json(*guidance.corner);
            }
            if (guidance.x_axis.has_value()) {
                body["x_axis"] = point_json(*guidance.x_axis);
            }
            if (guidance.y_axis.has_value()) {
                body["y_axis"] = point_json(*guidance.y_axis);
            }
            if (guidance.z_axis.has_value()) {
                body["z_axis"] = point_json(*guidance.z_axis);
            }
            if (guidance.confidence.has_value()) {
                body["confidence"] = *guidance.confidence;
            }
            if (guidance.waiting_reason.has_value()) {
                body["waiting_reason"] = *guidance.waiting_reason;
            }
        }
        auto serialized = body.dump();
        if (serialized.size() > maximum_bytes) {
            return failure(ErrorCode::ResourceExhausted,
                           "application result body exceeds the configured limit");
        }
        return Result<std::string>::success(std::move(serialized));
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::ResourceExhausted,
                       "unable to allocate application result body");
    } catch (const std::length_error&) {
        return failure(ErrorCode::ResourceExhausted,
                       "application result body exceeds the platform limit");
    } catch (const std::exception&) {
        return failure(ErrorCode::InternalError,
                       "unable to serialize application result body");
    }
}

}  // namespace iaisf::application
