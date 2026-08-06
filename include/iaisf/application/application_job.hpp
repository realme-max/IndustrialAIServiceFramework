#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "iaisf/application/application_identity.hpp"
#include "iaisf/application/application_job_id.hpp"
#include "iaisf/application/application_job_state.hpp"
#include "iaisf/application/application_submission.hpp"
#include "iaisf/application/artifact_ref.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

inline constexpr std::size_t kMinApplicationJobInputArtifacts = 1U;
inline constexpr std::size_t kMaxApplicationJobInputArtifacts = 16U;

using ApplicationJobTimePoint = std::chrono::system_clock::time_point;

struct ApplicationJobCreateRequest {
    ApplicationJobId job_id;
    IndustrialApplication application;
    ScenePhase scene_phase;
    ApplicationSubmissionSpec submission;
    ApplicationJobTimePoint created_at;
    std::vector<ArtifactRef> input_artifacts;
};

class ApplicationJobRepositoryTestAccess;

/**
 * Immutable public value snapshot of one application job.
 *
 * Instances can only be created in Accepted/version-1 form or derived through
 * the centralized transition matrix. No mutable repository reference escapes.
 */
class ApplicationJobSnapshot {
public:
    [[nodiscard]] static Result<ApplicationJobSnapshot> create(
        const ApplicationJobCreateRequest& request);

    [[nodiscard]] Result<ApplicationJobSnapshot> transitioned(
        ApplicationJobState target_state,
        ApplicationJobTimePoint updated_at) const;

    ApplicationJobSnapshot(const ApplicationJobSnapshot&) = default;
    ApplicationJobSnapshot& operator=(const ApplicationJobSnapshot& other);

    /** Copy-preserving move keeps both snapshots valid and independently owned. */
    ApplicationJobSnapshot(ApplicationJobSnapshot&& other);
    ApplicationJobSnapshot& operator=(ApplicationJobSnapshot&& other);

    [[nodiscard]] const ApplicationJobId& job_id() const noexcept;
    [[nodiscard]] IndustrialApplication application() const noexcept;
    [[nodiscard]] ScenePhase scene_phase() const noexcept;
    [[nodiscard]] ApplicationJobState state() const noexcept;
    [[nodiscard]] std::uint64_t version() const noexcept;
    [[nodiscard]] ApplicationJobTimePoint created_at() const noexcept;
    [[nodiscard]] ApplicationJobTimePoint updated_at() const noexcept;
    [[nodiscard]] const ApplicationSubmissionSpec& submission() const noexcept;
    [[nodiscard]] const std::vector<ArtifactRef>& input_artifacts() const noexcept;

private:
    friend class ApplicationJobRepositoryTestAccess;
    friend class InMemoryApplicationJobRepository;

    ApplicationJobSnapshot(
        ApplicationJobId job_id,
        IndustrialApplication application,
        ScenePhase scene_phase,
        ApplicationSubmissionSpec submission,
        ApplicationJobState state,
        std::uint64_t version,
        ApplicationJobTimePoint created_at,
        ApplicationJobTimePoint updated_at,
        std::vector<ArtifactRef> input_artifacts);

    void swap(ApplicationJobSnapshot& other) noexcept;

    ApplicationJobId job_id_;
    IndustrialApplication application_;
    ScenePhase scene_phase_;
    ApplicationSubmissionSpec submission_;
    ApplicationJobState state_;
    std::uint64_t version_;
    ApplicationJobTimePoint created_at_;
    ApplicationJobTimePoint updated_at_;
    std::vector<ArtifactRef> input_artifacts_;
};

}  // namespace iaisf::application
