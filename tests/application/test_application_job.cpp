#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "iaisf/application/application_job.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] ApplicationJobId job_id(const std::string& text) {
    auto parsed = ApplicationJobId::parse(text);
    return std::move(parsed).value();
}

[[nodiscard]] ArtifactRef artifact(const std::string& id) {
    return ArtifactRef{
        id,
        std::string(kSha256HexBytes, 'a'),
        128U,
        "point_cloud",
        "application/vnd.iaisf.pointcloud.xyz-f32le",
        std::string{"workpiece"},
        std::string{"mm"},
        4U,
    };
}

[[nodiscard]] ApplicationSubmissionSpec inspection_spec() {
    auto outputs = InspectionRequestedOutputs::create(true, true);
    EXPECT_TRUE(outputs);
    auto submission = WeldInspectionSubmission::create(
        std::move(outputs).value());
    EXPECT_TRUE(submission);
    auto spec = ApplicationSubmissionSpec::create(
        std::move(submission).value());
    EXPECT_TRUE(spec);
    return std::move(spec).value();
}

[[nodiscard]] ApplicationJobCreateRequest request(
    const std::string& id,
    const IndustrialApplication application =
        IndustrialApplication::WeldInspection,
    const ScenePhase phase = ScenePhase::PostWeld) {
    return ApplicationJobCreateRequest{
        job_id(id),
        application,
        phase,
        inspection_spec(),
        ApplicationJobTimePoint{std::chrono::seconds{100}},
        {artifact("input-1")},
    };
}

TEST(ApplicationJobSnapshotTest, CreationProducesAcceptedVersionOneSnapshot) {
    const auto created = ApplicationJobSnapshot::create(request("job-1"));
    ASSERT_TRUE(created);
    EXPECT_EQ(created.value().job_id().value(), "job-1");
    EXPECT_EQ(
        created.value().application(),
        IndustrialApplication::WeldInspection);
    EXPECT_EQ(created.value().scene_phase(), ScenePhase::PostWeld);
    EXPECT_EQ(created.value().state(), ApplicationJobState::Accepted);
    EXPECT_EQ(created.value().version(), 1U);
    EXPECT_EQ(created.value().created_at(), created.value().updated_at());
    ASSERT_NE(created.value().submission().inspection(), nullptr);
    EXPECT_TRUE(created.value().submission().inspection()->outputs()
                    .requests_segmentation());
    EXPECT_TRUE(created.value().submission().inspection()->outputs()
                    .requests_geometry());
    ASSERT_EQ(created.value().input_artifacts().size(), 1U);
}

TEST(ApplicationJobSnapshotTest, CreationOwnsAnIndependentInputSnapshot) {
    auto source = request("job-copy");
    const auto created = ApplicationJobSnapshot::create(source);
    ASSERT_TRUE(created);
    source.input_artifacts.front().artifact_id = "changed";
    source.input_artifacts.clear();
    EXPECT_EQ(created.value().input_artifacts().size(), 1U);
    EXPECT_EQ(
        created.value().input_artifacts().front().artifact_id,
        "input-1");
}

TEST(ApplicationJobSnapshotTest, MovedIdCanBeReusedForValidSnapshots) {
    auto parsed = ApplicationJobId::parse("job-reused-source");
    ASSERT_TRUE(parsed);
    auto source = parsed.value();
    ApplicationJobId destination{std::move(source)};

    auto source_request = request("temporary-source");
    source_request.job_id = source;
    const auto source_snapshot = ApplicationJobSnapshot::create(source_request);
    ASSERT_TRUE(source_snapshot);
    EXPECT_EQ(source_snapshot.value().job_id().value(), "job-reused-source");
    EXPECT_FALSE(source_snapshot.value().input_artifacts().empty());

    auto destination_request = request("temporary-destination");
    destination_request.job_id = destination;
    const auto destination_snapshot =
        ApplicationJobSnapshot::create(destination_request);
    ASSERT_TRUE(destination_snapshot);
    EXPECT_EQ(
        destination_snapshot.value().job_id().value(),
        "job-reused-source");
}

TEST(ApplicationJobSnapshotTest, MoveConstructionPreservesSourceInvariants) {
    auto source_result = ApplicationJobSnapshot::create(
        request("job-snapshot-move"));
    ASSERT_TRUE(source_result);
    auto source = source_result.value();
    ApplicationJobSnapshot destination{std::move(source)};

    EXPECT_TRUE(source.job_id().valid());
    EXPECT_EQ(source.job_id().value(), "job-snapshot-move");
    EXPECT_EQ(source.input_artifacts().size(), 1U);
    EXPECT_EQ(destination.job_id(), source.job_id());
    EXPECT_EQ(destination.input_artifacts().size(), 1U);
    EXPECT_EQ(destination.submission(), source.submission());

    const auto transitioned = source.transitioned(
        ApplicationJobState::Queued,
        ApplicationJobTimePoint{std::chrono::seconds{101}});
    ASSERT_TRUE(transitioned);
    EXPECT_EQ(transitioned.value().version(), 2U);
    EXPECT_EQ(transitioned.value().submission(), source.submission());
}

TEST(ApplicationJobSnapshotTest, MoveAssignmentPreservesSourceInvariants) {
    auto source_result = ApplicationJobSnapshot::create(
        request("job-snapshot-assignment-source"));
    auto destination_result = ApplicationJobSnapshot::create(
        request("job-snapshot-assignment-destination"));
    ASSERT_TRUE(source_result);
    ASSERT_TRUE(destination_result);
    auto source = source_result.value();
    auto destination = destination_result.value();

    destination = std::move(source);

    EXPECT_TRUE(source.job_id().valid());
    EXPECT_EQ(source.input_artifacts().size(), 1U);
    EXPECT_EQ(destination.job_id(), source.job_id());
    EXPECT_EQ(destination.input_artifacts().size(), 1U);
    EXPECT_EQ(destination.submission(), source.submission());
}

TEST(ApplicationJobSnapshotTest, AllocatingMoveOperationsAreNotNoexcept) {
    static_assert(
        !std::is_nothrow_move_constructible_v<ApplicationJobSnapshot>);
    static_assert(!std::is_nothrow_move_assignable_v<ApplicationJobSnapshot>);
    SUCCEED();
}

TEST(ApplicationJobSnapshotTest, RejectsCrossApplicationScenePairings) {
    EXPECT_FALSE(ApplicationJobSnapshot::create(request(
        "job-cross-1",
        IndustrialApplication::WeldInspection,
        ScenePhase::PreWeld)));
    EXPECT_FALSE(ApplicationJobSnapshot::create(request(
        "job-cross-2",
        IndustrialApplication::WeldingGuidance,
        ScenePhase::PostWeld)));
}

TEST(ApplicationJobSnapshotTest, EnforcesInputCountBounds) {
    auto empty = request("job-empty");
    empty.input_artifacts.clear();
    EXPECT_FALSE(ApplicationJobSnapshot::create(empty));

    auto maximum = request("job-maximum");
    maximum.input_artifacts.clear();
    for (std::size_t index = 0U;
         index < kMaxApplicationJobInputArtifacts;
         ++index) {
        maximum.input_artifacts.push_back(
            artifact("input-" + std::to_string(index)));
    }
    EXPECT_TRUE(ApplicationJobSnapshot::create(maximum));
    maximum.input_artifacts.push_back(artifact("input-overflow"));
    EXPECT_FALSE(ApplicationJobSnapshot::create(maximum));
}

TEST(ApplicationJobSnapshotTest, RejectsInvalidAndDuplicateArtifacts) {
    auto invalid = request("job-invalid-artifact");
    invalid.input_artifacts.front().sha256 = "invalid";
    EXPECT_FALSE(ApplicationJobSnapshot::create(invalid));

    auto duplicate = request("job-duplicate-artifact");
    duplicate.input_artifacts.push_back(duplicate.input_artifacts.front());
    EXPECT_FALSE(ApplicationJobSnapshot::create(duplicate));
}

TEST(ApplicationJobSnapshotTest, TransitionPreservesIdentityAndIncrementsOnce) {
    const auto created = ApplicationJobSnapshot::create(request("job-transition"));
    ASSERT_TRUE(created);
    const auto update_time =
        ApplicationJobTimePoint{std::chrono::seconds{101}};
    const auto transitioned = created.value().transitioned(
        ApplicationJobState::Queued, update_time);
    ASSERT_TRUE(transitioned);
    EXPECT_EQ(transitioned.value().job_id(), created.value().job_id());
    EXPECT_EQ(transitioned.value().state(), ApplicationJobState::Queued);
    EXPECT_EQ(transitioned.value().version(), 2U);
    EXPECT_EQ(transitioned.value().submission(), created.value().submission());
    EXPECT_EQ(transitioned.value().created_at(), created.value().created_at());
    EXPECT_EQ(transitioned.value().updated_at(), update_time);
}

TEST(ApplicationJobSnapshotTest, TransitionRejectsInvalidStateAndTimeRegression) {
    const auto created = ApplicationJobSnapshot::create(request("job-reject"));
    ASSERT_TRUE(created);
    EXPECT_FALSE(created.value().transitioned(
        ApplicationJobState::Running,
        ApplicationJobTimePoint{std::chrono::seconds{101}}));
    EXPECT_FALSE(created.value().transitioned(
        ApplicationJobState::Queued,
        ApplicationJobTimePoint{std::chrono::seconds{99}}));
}

TEST(ApplicationJobSnapshotTest, InspectionCannotEnterWaitingHuman) {
    auto created = ApplicationJobSnapshot::create(request("job-inspection"));
    ASSERT_TRUE(created);
    auto queued = created.value().transitioned(
        ApplicationJobState::Queued,
        ApplicationJobTimePoint{std::chrono::seconds{101}});
    ASSERT_TRUE(queued);
    auto dispatching = queued.value().transitioned(
        ApplicationJobState::Dispatching,
        ApplicationJobTimePoint{std::chrono::seconds{102}});
    ASSERT_TRUE(dispatching);
    auto running = dispatching.value().transitioned(
        ApplicationJobState::Running,
        ApplicationJobTimePoint{std::chrono::seconds{103}});
    ASSERT_TRUE(running);
    EXPECT_FALSE(running.value().transitioned(
        ApplicationJobState::WaitingHuman,
        ApplicationJobTimePoint{std::chrono::seconds{104}}));
}

}  // namespace
}  // namespace iaisf::application
