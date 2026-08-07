#include "iaisf/application/application_status_json.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

#include <nlohmann/json.hpp>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

Result<std::int64_t> epoch_milliseconds(const ApplicationJobTimePoint time) {
    const auto since_epoch = time.time_since_epoch();
    if (since_epoch < ApplicationJobTimePoint::duration::zero()) {
        return Result<std::int64_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application job timestamp is before the Unix epoch"));
    }
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        since_epoch);
    if (millis.count() < 0 ||
        static_cast<unsigned long long>(millis.count()) >
            static_cast<unsigned long long>(
                std::numeric_limits<std::int64_t>::max())) {
        return Result<std::int64_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application job timestamp is not representable"));
    }
    return Result<std::int64_t>::success(millis.count());
}

}  // namespace

Result<std::string> application_job_status_json(
    const ApplicationJobSnapshot& snapshot,
    const std::size_t maximum_bytes) {
    try {
        if (maximum_bytes == 0U ||
            maximum_bytes > kMaxApplicationStatusBodyBytes ||
            !snapshot.job_id().valid() ||
            to_string(snapshot.application()) == "unknown" ||
            to_string(snapshot.scene_phase()) == "unknown" ||
            to_string(snapshot.state()) == "unknown" ||
            snapshot.version() == 0U) {
            return Result<std::string>::failure(make_error(
                ErrorCode::InvalidArgument,
                "application job snapshot is invalid for public status"));
        }
        const auto created = epoch_milliseconds(snapshot.created_at());
        const auto updated = epoch_milliseconds(snapshot.updated_at());
        if (!created || !updated || updated.value() < created.value()) {
            return Result<std::string>::failure(make_error(
                ErrorCode::InvalidArgument,
                "application job timestamp is invalid for public status"));
        }
        const auto valid_identity = validate_application_scene(
            snapshot.application(), snapshot.scene_phase());
        if (!valid_identity) {
            return Result<std::string>::failure(make_error(
                ErrorCode::InvalidArgument,
                "application job identity is invalid for public status"));
        }
        const auto valid_submission = snapshot.submission().validate_for(
            snapshot.application(), snapshot.scene_phase());
        if (!valid_submission ||
            snapshot.input_artifacts().size() <
                kMinApplicationJobInputArtifacts ||
            snapshot.input_artifacts().size() >
                kMaxApplicationJobInputArtifacts) {
            return Result<std::string>::failure(make_error(
                ErrorCode::InvalidArgument,
                "application job snapshot payload is invalid for public status"));
        }

        const std::string status_url =
            snapshot.application() == IndustrialApplication::WeldInspection
                ? "/api/weld-inspection/v1/jobs/" +
                      std::string(snapshot.job_id().value())
                : "/api/welding-guidance/v1/jobs/" +
                      std::string(snapshot.job_id().value());
        nlohmann::ordered_json body{
            {"schema_version", "1.0"},
            {"job_id", std::string(snapshot.job_id().value())},
            {"application", to_string(snapshot.application())},
            {"phase", to_string(snapshot.scene_phase())},
            {"state", to_string(snapshot.state())},
            {"version", snapshot.version()},
            {"created_at", created.value()},
            {"updated_at", updated.value()},
            {"status_url", status_url}};
        auto serialized = body.dump();
        if (serialized.size() > maximum_bytes) {
            return Result<std::string>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "application status body exceeds the configured limit"));
        }
        return Result<std::string>::success(std::move(serialized));
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate application status body"));
    } catch (const std::length_error&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "application status body exceeds the platform size limit"));
    } catch (const std::exception&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::InternalError,
            "unable to serialize application status body"));
    }
}

}  // namespace iaisf::application
