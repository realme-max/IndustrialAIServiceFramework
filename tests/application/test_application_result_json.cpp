#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "iaisf/application/application_job.hpp"
#include "iaisf/application/application_result_json.hpp"

namespace iaisf::application {
namespace {

ApplicationJobId id() {
    auto parsed = ApplicationJobId::parse("job-result");
    EXPECT_TRUE(parsed);
    return std::move(parsed).value();
}

ArtifactRef artifact() {
    return ArtifactRef{
        "input-1", std::string(kSha256HexBytes, 'a'), 12U, "point_cloud",
        "application/vnd.iaisf.pointcloud.xyz-f32le", std::string{"workpiece"},
        std::string{"mm"}, 1U};
}

ApplicationSubmissionSpec spec() {
    auto outputs = InspectionRequestedOutputs::create(true, true);
    auto submission = WeldInspectionSubmission::create(std::move(outputs).value());
    auto result = ApplicationSubmissionSpec::create(std::move(submission).value());
    return std::move(result).value();
}

ApplicationJobSnapshot snapshot() {
    const auto time = ApplicationJobTimePoint{std::chrono::seconds{100}};
    auto created = ApplicationJobSnapshot::create(
        ApplicationJobCreateRequest{id(), IndustrialApplication::WeldInspection,
                                    ScenePhase::PostWeld, spec(), time, {artifact()}});
    EXPECT_TRUE(created);
    auto queued = created.value().transitioned(ApplicationJobState::Queued,
                                               time + std::chrono::seconds{1});
    auto dispatching = queued.value().transitioned(ApplicationJobState::Dispatching,
                                                   time + std::chrono::seconds{2});
    auto running = dispatching.value().transitioned(ApplicationJobState::Running,
                                                    time + std::chrono::seconds{3});
    return std::move(running).value();
}

TEST(ApplicationResultJsonTest, EmitsBoundedInspectionProjection) {
    WeldInspectionResult result;
    auto output = artifact();
    output.artifact_id = "output-1";
    result.output_artifacts.push_back(output);
    result.weld_point_count = 1U;
    result.weld_ratio = 0.25;
    result.length_mm = 10.0;
    result.inference_time_ms = 2.0;
    result.total_time_ms = 3.0;
    auto completed = snapshot().completed(
        ApplicationExecutionResult{result},
        ApplicationJobTimePoint{std::chrono::seconds{104}});
    ASSERT_TRUE(completed);
    const auto json = application_execution_result_json(completed.value());
    ASSERT_TRUE(json);
    EXPECT_NE(json.value().find("quality_assessment"), std::string::npos);
    EXPECT_NE(json.value().find("not_implemented"), std::string::npos);
    EXPECT_EQ(json.value().find("input-1"), std::string::npos);
}

TEST(ApplicationResultJsonTest, EnforcesBodyLimit) {
    WeldInspectionResult result;
    auto output = artifact();
    output.artifact_id = "output-1";
    result.output_artifacts.push_back(output);
    result.weld_point_count = 1U;
    auto completed = snapshot().completed(
        ApplicationExecutionResult{result},
        ApplicationJobTimePoint{std::chrono::seconds{104}});
    ASSERT_TRUE(completed);
    EXPECT_FALSE(application_execution_result_json(completed.value(), 1U));
}

}  // namespace
}  // namespace iaisf::application
