#include "iaisf/application/application_job.hpp"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] Result<void> validate_inputs(
    const std::vector<ArtifactRef>& artifacts) {
    if (artifacts.size() < kMinApplicationJobInputArtifacts ||
        artifacts.size() > kMaxApplicationJobInputArtifacts) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application job input artifact count is invalid"));
    }
    for (std::size_t index = 0U; index < artifacts.size(); ++index) {
        const auto valid = validate_artifact_ref(artifacts[index]);
        if (!valid) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "application job contains an invalid artifact"));
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (artifacts[previous].artifact_id == artifacts[index].artifact_id) {
                return Result<void>::failure(make_error(
                    ErrorCode::InvalidArgument,
                    "application job contains a duplicate artifact id"));
            }
        }
    }
    return Result<void>::success();
}

[[nodiscard]] bool valid_state(const ApplicationJobState state) noexcept {
    return to_string(state) != "unknown";
}

[[nodiscard]] Result<void> validate_snapshot_invariants(
    const ApplicationJobId& job_id,
    const IndustrialApplication application,
    const ScenePhase scene_phase,
    const ApplicationJobState state,
    const std::uint64_t version,
    const ApplicationJobTimePoint created_at,
    const ApplicationJobTimePoint updated_at,
    const std::vector<ArtifactRef>& input_artifacts) {
    if (!job_id.valid()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application job id is invalid"));
    }
    const auto valid_identity = validate_application_scene(
        application, scene_phase);
    if (!valid_identity) {
        return valid_identity;
    }
    if (!valid_state(state) || version == 0U || updated_at < created_at) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application job snapshot invariant is invalid"));
    }
    return validate_inputs(input_artifacts);
}

}  // namespace

ApplicationJobSnapshot::ApplicationJobSnapshot(
    ApplicationJobId job_id,
    const IndustrialApplication application,
    const ScenePhase scene_phase,
    const ApplicationJobState state,
    const std::uint64_t version,
    const ApplicationJobTimePoint created_at,
    const ApplicationJobTimePoint updated_at,
    std::vector<ArtifactRef> input_artifacts)
    : job_id_(std::move(job_id)),
      application_(application),
      scene_phase_(scene_phase),
      state_(state),
      version_(version),
      created_at_(created_at),
      updated_at_(updated_at),
      input_artifacts_(std::move(input_artifacts)) {}

ApplicationJobSnapshot::ApplicationJobSnapshot(ApplicationJobSnapshot&& other)
    : ApplicationJobSnapshot(
          static_cast<const ApplicationJobSnapshot&>(other)) {}

ApplicationJobSnapshot& ApplicationJobSnapshot::operator=(
    const ApplicationJobSnapshot& other) {
    if (this != &other) {
        ApplicationJobSnapshot copy{other};
        swap(copy);
    }
    return *this;
}

ApplicationJobSnapshot& ApplicationJobSnapshot::operator=(
    ApplicationJobSnapshot&& other) {
    if (this != &other) {
        *this = static_cast<const ApplicationJobSnapshot&>(other);
    }
    return *this;
}

void ApplicationJobSnapshot::swap(ApplicationJobSnapshot& other) noexcept {
    job_id_.swap(other.job_id_);
    using std::swap;
    swap(application_, other.application_);
    swap(scene_phase_, other.scene_phase_);
    swap(state_, other.state_);
    swap(version_, other.version_);
    swap(created_at_, other.created_at_);
    swap(updated_at_, other.updated_at_);
    input_artifacts_.swap(other.input_artifacts_);
}

Result<ApplicationJobSnapshot> ApplicationJobSnapshot::create(
    const ApplicationJobCreateRequest& request) {
    const auto valid = validate_snapshot_invariants(
        request.job_id,
        request.application,
        request.scene_phase,
        ApplicationJobState::Accepted,
        1U,
        request.created_at,
        request.created_at,
        request.input_artifacts);
    if (!valid) {
        return Result<ApplicationJobSnapshot>::failure(valid.error());
    }
    try {
        return Result<ApplicationJobSnapshot>::success(ApplicationJobSnapshot{
            request.job_id,
            request.application,
            request.scene_phase,
            ApplicationJobState::Accepted,
            1U,
            request.created_at,
            request.created_at,
            request.input_artifacts});
    } catch (const std::bad_alloc&) {
        return Result<ApplicationJobSnapshot>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate application job snapshot"));
    } catch (const std::length_error&) {
        return Result<ApplicationJobSnapshot>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "application job snapshot exceeds the platform size limit"));
    }
}

Result<ApplicationJobSnapshot> ApplicationJobSnapshot::transitioned(
    const ApplicationJobState target_state,
    const ApplicationJobTimePoint updated_at) const {
    const auto source_valid = validate_snapshot_invariants(
        job_id_,
        application_,
        scene_phase_,
        state_,
        version_,
        created_at_,
        updated_at_,
        input_artifacts_);
    if (!source_valid) {
        return Result<ApplicationJobSnapshot>::failure(source_valid.error());
    }
    if (updated_at < updated_at_) {
        return Result<ApplicationJobSnapshot>::failure(make_error(
            ErrorCode::InvalidArgument,
            "application job timestamp cannot move backwards"));
    }
    if (version_ == std::numeric_limits<std::uint64_t>::max()) {
        return Result<ApplicationJobSnapshot>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "application job version space is exhausted"));
    }
    const auto valid = validate_transition(state_, target_state, application_);
    if (!valid) {
        return Result<ApplicationJobSnapshot>::failure(valid.error());
    }
    try {
        auto updated = *this;
        updated.state_ = target_state;
        ++updated.version_;
        updated.updated_at_ = updated_at;
        return Result<ApplicationJobSnapshot>::success(std::move(updated));
    } catch (const std::bad_alloc&) {
        return Result<ApplicationJobSnapshot>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate transitioned application job snapshot"));
    } catch (const std::length_error&) {
        return Result<ApplicationJobSnapshot>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "transitioned application job snapshot exceeds the platform size limit"));
    }
}

const ApplicationJobId& ApplicationJobSnapshot::job_id() const noexcept {
    return job_id_;
}

IndustrialApplication ApplicationJobSnapshot::application() const noexcept {
    return application_;
}

ScenePhase ApplicationJobSnapshot::scene_phase() const noexcept {
    return scene_phase_;
}

ApplicationJobState ApplicationJobSnapshot::state() const noexcept {
    return state_;
}

std::uint64_t ApplicationJobSnapshot::version() const noexcept {
    return version_;
}

ApplicationJobTimePoint ApplicationJobSnapshot::created_at() const noexcept {
    return created_at_;
}

ApplicationJobTimePoint ApplicationJobSnapshot::updated_at() const noexcept {
    return updated_at_;
}

const std::vector<ArtifactRef>& ApplicationJobSnapshot::input_artifacts() const noexcept {
    return input_artifacts_;
}

}  // namespace iaisf::application
