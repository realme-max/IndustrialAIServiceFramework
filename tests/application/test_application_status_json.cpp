#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "iaisf/application/application_status_json.hpp"

namespace iaisf::application {

class ApplicationJobRepositoryTestAccess {
public:
    static ApplicationJobSnapshot with_times(
        const ApplicationJobSnapshot& source,
        const ApplicationJobTimePoint created,
        const ApplicationJobTimePoint updated) {
        return ApplicationJobSnapshot{
            source.job_id(),
            source.application(),
            source.scene_phase(),
            source.submission(),
            source.state(),
            source.version(),
            created,
            updated,
            source.input_artifacts()};
    }
};

namespace {

ApplicationSubmissionSpec inspection_spec() {
    auto outputs = InspectionRequestedOutputs::create(true, true);
    EXPECT_TRUE(outputs);
    auto submission = WeldInspectionSubmission::create(outputs.value());
    EXPECT_TRUE(submission);
    auto spec = ApplicationSubmissionSpec::create(submission.value());
    EXPECT_TRUE(spec);
    return spec.value();
}

ApplicationSubmissionSpec guidance_spec() {
    auto weld_type = WeldTypeRequest::create(
        WeldTypeMode::Requested, RequestedWeldType::Corner);
    EXPECT_TRUE(weld_type);
    auto guidance = WeldingGuidanceSubmission::create(
        weld_type.value(), HumanCheckpointPolicy::Required);
    EXPECT_TRUE(guidance);
    auto spec = ApplicationSubmissionSpec::create(guidance.value());
    EXPECT_TRUE(spec);
    return spec.value();
}

ApplicationJobSnapshot snapshot() {
    auto id = ApplicationJobId::parse("wi_example");
    EXPECT_TRUE(id);
    ApplicationJobCreateRequest request{
        id.value(),
        IndustrialApplication::WeldInspection,
        ScenePhase::PostWeld,
        inspection_spec(),
        std::chrono::system_clock::time_point{std::chrono::milliseconds{1785988800123LL}},
        {ArtifactRef{"input-001",
                     "0000000000000000000000000000000000000000000000000000000000000000",
                     1440U,
                     "point_cloud",
                     "application/vnd.iaisf.pointcloud.xyz-f32le",
                     std::string{"camera"},
                     std::string{"mm"},
                     120U}}};
    auto result = ApplicationJobSnapshot::create(request);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

ApplicationJobSnapshot guidance_snapshot() {
    auto id = ApplicationJobId::parse("wg_example");
    EXPECT_TRUE(id);
    ApplicationJobCreateRequest request{
        id.value(),
        IndustrialApplication::WeldingGuidance,
        ScenePhase::PreWeld,
        guidance_spec(),
        std::chrono::system_clock::time_point{
            std::chrono::milliseconds{1785988800123LL}},
        {ArtifactRef{"input-001",
                     "0000000000000000000000000000000000000000000000000000000000000000",
                     1440U,
                     "point_cloud",
                     "application/vnd.iaisf.pointcloud.xyz-f32le",
                     std::string{"camera"},
                     std::string{"mm"},
                     120U}}};
    auto result = ApplicationJobSnapshot::create(request);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

TEST(ApplicationStatusJsonTest, ProducesStableInspectionBodyAndCanonicalUrl) {
    const auto result = application_job_status_json(snapshot());
    ASSERT_TRUE(result);
    const auto body = nlohmann::json::parse(result.value());
    EXPECT_EQ(body.at("schema_version"), "1.0");
    EXPECT_EQ(body.at("job_id"), "wi_example");
    EXPECT_EQ(body.at("application"), "weld_inspection");
    EXPECT_EQ(body.at("phase"), "post_weld");
    EXPECT_EQ(body.at("state"), "accepted");
    EXPECT_EQ(body.at("created_at"), 1785988800123LL);
    EXPECT_EQ(body.at("status_url"), "/api/weld-inspection/v1/jobs/wi_example");
    EXPECT_FALSE(body.contains("input_artifacts"));
    EXPECT_FALSE(body.contains("submission"));
    EXPECT_FALSE(body.contains("requested_outputs"));
    EXPECT_FALSE(body.contains("weld_type"));
    EXPECT_FALSE(body.contains("review_policy"));
    EXPECT_FALSE(body.contains("quality"));
    EXPECT_FALSE(body.contains("worker"));
    EXPECT_EQ(body.dump().size(), body.dump().length());
    EXPECT_EQ(result.value().find("HTTP/"), std::string::npos);
}

TEST(ApplicationStatusJsonTest, ProjectsLaterStateWithoutChangingSchema) {
    auto current = snapshot();
    for (const auto target : std::array<ApplicationJobState, 4>{
             ApplicationJobState::Queued,
             ApplicationJobState::Dispatching,
             ApplicationJobState::Running,
             ApplicationJobState::Succeeded}) {
        const auto transitioned = current.transitioned(
            target, current.updated_at() + std::chrono::milliseconds{1});
        ASSERT_TRUE(transitioned);
        const auto result = application_job_status_json(transitioned.value());
        ASSERT_TRUE(result);
        const auto body = nlohmann::json::parse(result.value());
        EXPECT_EQ(body.at("state"), std::string{to_string(target)});
        EXPECT_EQ(body.at("version"), current.version() + 1U);
        current = std::move(transitioned).value();
    }

    const auto project_state = [](const ApplicationJobSnapshot& source,
                                  const ApplicationJobState target) {
        const auto transitioned = source.transitioned(
            target, source.updated_at() + std::chrono::milliseconds{1});
        EXPECT_TRUE(transitioned);
        if (!transitioned) {
            return;
        }
        const auto result = application_job_status_json(transitioned.value());
        ASSERT_TRUE(result);
        const auto body = nlohmann::json::parse(result.value());
        EXPECT_EQ(body.at("state"), std::string{to_string(target)});
        EXPECT_EQ(
            body.at("status_url"),
            source.application() == IndustrialApplication::WeldInspection
                ? "/api/weld-inspection/v1/jobs/wi_example"
                : "/api/welding-guidance/v1/jobs/wg_example");
    };
    project_state(snapshot(), ApplicationJobState::Failed);
    const auto queued = snapshot().transitioned(
        ApplicationJobState::Queued,
        snapshot().updated_at() + std::chrono::milliseconds{1});
    ASSERT_TRUE(queued);
    project_state(queued.value(), ApplicationJobState::TimedOut);
    const auto dispatching = queued.value().transitioned(
        ApplicationJobState::Dispatching,
        queued.value().updated_at() + std::chrono::milliseconds{1});
    ASSERT_TRUE(dispatching);
    project_state(dispatching.value(), ApplicationJobState::WorkerLost);

    const auto guidance = guidance_snapshot();
    const auto guidance_queued = guidance.transitioned(
        ApplicationJobState::Queued,
        guidance.updated_at() + std::chrono::milliseconds{1});
    ASSERT_TRUE(guidance_queued);
    const auto guidance_dispatching = guidance_queued.value().transitioned(
        ApplicationJobState::Dispatching,
        guidance_queued.value().updated_at() + std::chrono::milliseconds{1});
    ASSERT_TRUE(guidance_dispatching);
    const auto guidance_running = guidance_dispatching.value().transitioned(
        ApplicationJobState::Running,
        guidance_dispatching.value().updated_at() + std::chrono::milliseconds{1});
    ASSERT_TRUE(guidance_running);
    project_state(guidance_running.value(), ApplicationJobState::WaitingHuman);
    project_state(guidance_running.value(), ApplicationJobState::Cancelling);
    const auto cancelling = guidance_running.value().transitioned(
        ApplicationJobState::Cancelling,
        guidance_running.value().updated_at() + std::chrono::milliseconds{1});
    ASSERT_TRUE(cancelling);
    project_state(cancelling.value(), ApplicationJobState::Cancelled);

    const auto guidance_result = application_job_status_json(guidance_snapshot());
    ASSERT_TRUE(guidance_result);
    const auto guidance_body = nlohmann::json::parse(guidance_result.value());
    EXPECT_EQ(guidance_body.at("application"), "welding_guidance");
    EXPECT_EQ(guidance_body.at("phase"), "pre_weld");
    EXPECT_EQ(
        guidance_body.at("status_url"),
        "/api/welding-guidance/v1/jobs/wg_example");
}

TEST(ApplicationStatusJsonTest, EnforcesResponseLimitAndNoHttpHeaders) {
    const auto normal = application_job_status_json(snapshot());
    ASSERT_TRUE(normal);
    EXPECT_EQ(normal.value().size(), normal.value().length());
    EXPECT_TRUE(application_job_status_json(snapshot(), normal.value().size()));
    EXPECT_FALSE(application_job_status_json(snapshot(), normal.value().size() - 1U));
    EXPECT_TRUE(application_job_status_json(snapshot(), kMaxApplicationStatusBodyBytes));
    EXPECT_FALSE(application_job_status_json(
        snapshot(), kMaxApplicationStatusBodyBytes + 1U));

    const auto negative = ApplicationJobRepositoryTestAccess::with_times(
        snapshot(),
        std::chrono::system_clock::time_point{std::chrono::milliseconds{-1}},
        std::chrono::system_clock::time_point{std::chrono::milliseconds{-1}});
    const auto negative_result = application_job_status_json(negative);
    ASSERT_FALSE(negative_result);
    EXPECT_EQ(negative_result.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace iaisf::application
