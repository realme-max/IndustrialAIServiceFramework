#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "iaisf/application/in_memory_application_job_repository.hpp"

namespace iaisf::application {
namespace {

ApplicationJobId id(const std::string& text) {
    auto parsed = ApplicationJobId::parse(text);
    EXPECT_TRUE(parsed);
    return std::move(parsed).value();
}

ArtifactRef artifact(const std::string& text) {
    return ArtifactRef{text, std::string(kSha256HexBytes, 'a'), 12U,
                       "point_cloud", "application/vnd.iaisf.pointcloud.xyz-f32le",
                       std::string{"workpiece"}, std::string{"mm"}, 1U};
}

ApplicationSubmissionSpec inspection_spec() {
    auto outputs = InspectionRequestedOutputs::create(true, false);
    auto submission = WeldInspectionSubmission::create(std::move(outputs).value());
    auto spec = ApplicationSubmissionSpec::create(std::move(submission).value());
    return std::move(spec).value();
}

ApplicationJobCreateRequest request(const std::string& text) {
    const auto time = ApplicationJobTimePoint{std::chrono::seconds{100}};
    return ApplicationJobCreateRequest{id(text), IndustrialApplication::WeldInspection,
                                       ScenePhase::PostWeld, inspection_spec(), time,
                                       {artifact("input-" + text)}};
}

ApplicationExecutionResult inspection_result() {
    WeldInspectionResult result;
    result.output_artifacts.push_back(artifact("output-1"));
    result.weld_point_count = 1U;
    result.weld_ratio = 0.5;
    result.length_mm = 1.0;
    result.inference_time_ms = 1.0;
    result.total_time_ms = 2.0;
    return result;
}

TEST(ApplicationJobRepositoryCompletionTest, CompletesRunningJobAtomically) {
    auto repository = InMemoryApplicationJobRepository::make(4U);
    ASSERT_TRUE(repository);
    ASSERT_TRUE(repository.value()->create(request("job-complete")));
    ASSERT_TRUE(repository.value()->transition(
        id("job-complete"), IndustrialApplication::WeldInspection, 1U,
        ApplicationJobState::Queued, ApplicationJobTimePoint{std::chrono::seconds{101}}));
    ASSERT_TRUE(repository.value()->transition(
        id("job-complete"), IndustrialApplication::WeldInspection, 2U,
        ApplicationJobState::Dispatching, ApplicationJobTimePoint{std::chrono::seconds{102}}));
    ASSERT_TRUE(repository.value()->transition(
        id("job-complete"), IndustrialApplication::WeldInspection, 3U,
        ApplicationJobState::Running, ApplicationJobTimePoint{std::chrono::seconds{103}}));
    const auto completed = repository.value()->complete(
        id("job-complete"), IndustrialApplication::WeldInspection, 4U,
        ApplicationJobState::Succeeded, inspection_result(),
        ApplicationJobTimePoint{std::chrono::seconds{104}});
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed.value().state(), ApplicationJobState::Succeeded);
    EXPECT_EQ(completed.value().version(), 5U);
    ASSERT_NE(completed.value().execution_result(), nullptr);
    EXPECT_FALSE(repository.value()->complete(
        id("job-complete"), IndustrialApplication::WeldInspection, 4U,
        ApplicationJobState::Succeeded, inspection_result(),
        ApplicationJobTimePoint{std::chrono::seconds{105}}));
}

TEST(ApplicationJobRepositoryCompletionTest, InvalidResultDoesNotChangeRecord) {
    auto repository = InMemoryApplicationJobRepository::make(4U);
    ASSERT_TRUE(repository);
    ASSERT_TRUE(repository.value()->create(request("job-invalid-complete")));
    ASSERT_TRUE(repository.value()->transition(
        id("job-invalid-complete"), IndustrialApplication::WeldInspection, 1U,
        ApplicationJobState::Queued, ApplicationJobTimePoint{std::chrono::seconds{101}}));
    ASSERT_TRUE(repository.value()->transition(
        id("job-invalid-complete"), IndustrialApplication::WeldInspection, 2U,
        ApplicationJobState::Dispatching, ApplicationJobTimePoint{std::chrono::seconds{102}}));
    ASSERT_TRUE(repository.value()->transition(
        id("job-invalid-complete"), IndustrialApplication::WeldInspection, 3U,
        ApplicationJobState::Running, ApplicationJobTimePoint{std::chrono::seconds{103}}));
    auto invalid = WeldInspectionResult{};
    const auto failed = repository.value()->complete(
        id("job-invalid-complete"), IndustrialApplication::WeldInspection, 4U,
        ApplicationJobState::Succeeded, ApplicationExecutionResult{invalid},
        ApplicationJobTimePoint{std::chrono::seconds{104}});
    EXPECT_FALSE(failed);
    const auto snapshot = repository.value()->get(
        id("job-invalid-complete"), IndustrialApplication::WeldInspection);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().state(), ApplicationJobState::Running);
    EXPECT_EQ(snapshot.value().version(), 4U);
    EXPECT_EQ(snapshot.value().execution_result(), nullptr);
}

}  // namespace
}  // namespace iaisf::application
