#include <gtest/gtest.h>

#include <string>

#include "iaisf/application/application_execution_result.hpp"

namespace iaisf::application {
namespace {

ArtifactRef output_artifact() {
    return ArtifactRef{
        "output-1",
        std::string(kSha256HexBytes, 'a'),
        12U,
        "point_cloud",
        "application/vnd.iaisf.pointcloud.xyz-f32le",
        std::string{"workpiece"},
        std::string{"mm"},
        1U};
}

TEST(ApplicationExecutionResultTest, InspectionRequiresExplicitMockQuality) {
    WeldInspectionResult result;
    result.output_artifacts.push_back(output_artifact());
    result.weld_point_count = 1U;
    result.weld_ratio = 0.5;
    result.length_mm = 10.0;
    result.inference_time_ms = 1.0;
    result.total_time_ms = 2.0;
    EXPECT_TRUE(validate_execution_result(
        ApplicationExecutionResult{result}, IndustrialApplication::WeldInspection));
    result.quality_assessment = "0.95";
    EXPECT_FALSE(validate_execution_result(
        ApplicationExecutionResult{result}, IndustrialApplication::WeldInspection));
}

TEST(ApplicationExecutionResultTest, GuidanceGeometryAndWaitingHumanAreFailClosed) {
    WeldingGuidanceResult completed;
    completed.output_artifacts.push_back(output_artifact());
    completed.weld_type = RequestedWeldType::L;
    completed.coordinate_frame = "workpiece";
    completed.start = ApplicationPoint3{0.0, 0.0, 0.0};
    completed.corner = ApplicationPoint3{1.0, 1.0, 0.0};
    completed.end = ApplicationPoint3{2.0, 0.0, 0.0};
    EXPECT_TRUE(validate_execution_result(
        ApplicationExecutionResult{completed}, IndustrialApplication::WeldingGuidance));

    WeldingGuidanceResult waiting;
    waiting.output_artifacts.push_back(output_artifact());
    waiting.coordinate_frame = "workpiece";
    waiting.disposition = GuidanceResultDisposition::WaitingHuman;
    waiting.waiting_reason = "review required";
    EXPECT_TRUE(validate_execution_result(
        ApplicationExecutionResult{waiting}, IndustrialApplication::WeldingGuidance));
    waiting.robot_execution_allowed = true;
    EXPECT_FALSE(validate_execution_result(
        ApplicationExecutionResult{waiting}, IndustrialApplication::WeldingGuidance));
}

}  // namespace
}  // namespace iaisf::application
