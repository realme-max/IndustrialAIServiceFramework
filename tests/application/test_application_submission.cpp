#include <gtest/gtest.h>

#include <optional>
#include <utility>

#include "iaisf/application/application_submission.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] ApplicationSubmissionSpec inspection_spec(
    const bool segmentation,
    const bool geometry) {
    auto outputs = InspectionRequestedOutputs::create(segmentation, geometry);
    EXPECT_TRUE(outputs);
    auto submission = WeldInspectionSubmission::create(
        std::move(outputs).value());
    EXPECT_TRUE(submission);
    auto spec = ApplicationSubmissionSpec::create(
        std::move(submission).value());
    EXPECT_TRUE(spec);
    return std::move(spec).value();
}

[[nodiscard]] ApplicationSubmissionSpec guidance_spec(
    const WeldTypeMode mode,
    const std::optional<RequestedWeldType> requested) {
    auto weld_type = WeldTypeRequest::create(mode, requested);
    EXPECT_TRUE(weld_type);
    auto submission = WeldingGuidanceSubmission::create(
        std::move(weld_type).value(), HumanCheckpointPolicy::Required);
    EXPECT_TRUE(submission);
    auto spec = ApplicationSubmissionSpec::create(
        std::move(submission).value());
    EXPECT_TRUE(spec);
    return std::move(spec).value();
}

TEST(ApplicationSubmissionTest, InspectionOutputsAreCanonicalAndBounded) {
    const auto segmentation = InspectionRequestedOutputs::create(true, false);
    const auto geometry = InspectionRequestedOutputs::create(false, true);
    const auto both = InspectionRequestedOutputs::create(true, true);
    ASSERT_TRUE(segmentation);
    ASSERT_TRUE(geometry);
    ASSERT_TRUE(both);
    EXPECT_TRUE(segmentation.value().requests_segmentation());
    EXPECT_FALSE(segmentation.value().requests_geometry());
    EXPECT_FALSE(geometry.value().requests_segmentation());
    EXPECT_TRUE(geometry.value().requests_geometry());
    EXPECT_EQ(
        InspectionRequestedOutputs::create(true, true).value(),
        InspectionRequestedOutputs::create(true, true).value());
    EXPECT_NE(segmentation.value(), geometry.value());
    EXPECT_FALSE(InspectionRequestedOutputs::create(false, false));
}

TEST(ApplicationSubmissionTest, InspectionFactorySupportsEveryValidCombination) {
    const auto segmentation = inspection_spec(true, false);
    const auto geometry = inspection_spec(false, true);
    const auto both = inspection_spec(true, true);
    EXPECT_EQ(segmentation.kind(), ApplicationSubmissionKind::WeldInspection);
    EXPECT_EQ(geometry.kind(), ApplicationSubmissionKind::WeldInspection);
    EXPECT_EQ(both.kind(), ApplicationSubmissionKind::WeldInspection);
    EXPECT_NE(segmentation, geometry);
    EXPECT_NE(geometry, both);
}

TEST(ApplicationSubmissionTest, GuidanceAutoHasNoRequestedType) {
    auto weld_type = WeldTypeRequest::create(WeldTypeMode::Auto, std::nullopt);
    ASSERT_TRUE(weld_type);
    EXPECT_EQ(weld_type.value().mode(), WeldTypeMode::Auto);
    EXPECT_FALSE(weld_type.value().requested_type().has_value());
    EXPECT_TRUE(WeldingGuidanceSubmission::create(
        std::move(weld_type).value(), HumanCheckpointPolicy::Required));
}

TEST(ApplicationSubmissionTest, GuidanceRequestedSupportsAllRequestedTypes) {
    for (const auto type : {RequestedWeldType::Straight,
                            RequestedWeldType::Corner,
                            RequestedWeldType::L}) {
        auto weld_type = WeldTypeRequest::create(
            WeldTypeMode::Requested, type);
        ASSERT_TRUE(weld_type);
        auto guidance = WeldingGuidanceSubmission::create(
            std::move(weld_type).value(), HumanCheckpointPolicy::Required);
        ASSERT_TRUE(guidance);
        auto spec = ApplicationSubmissionSpec::create(
            std::move(guidance).value());
        ASSERT_TRUE(spec);
        EXPECT_EQ(
            spec.value().kind(), ApplicationSubmissionKind::WeldingGuidance);
    }
}

TEST(ApplicationSubmissionTest, GuidanceInvalidCombinationsFailClosed) {
    EXPECT_FALSE(WeldTypeRequest::create(
        WeldTypeMode::Auto, RequestedWeldType::Straight));
    EXPECT_FALSE(WeldTypeRequest::create(
        WeldTypeMode::Requested, std::nullopt));
    EXPECT_FALSE(WeldTypeRequest::create(
        static_cast<WeldTypeMode>(99), std::nullopt));
    auto requested = WeldTypeRequest::create(
        WeldTypeMode::Requested, RequestedWeldType::Corner);
    ASSERT_TRUE(requested);
    EXPECT_FALSE(WeldingGuidanceSubmission::create(
        std::move(requested).value(),
        static_cast<HumanCheckpointPolicy>(99)));
}

TEST(ApplicationSubmissionTest, EnumStringsAreStableAndInvalidValuesAreUnknown) {
    EXPECT_EQ(to_string(ApplicationSubmissionKind::WeldInspection),
              "weld_inspection");
    EXPECT_EQ(to_string(WeldTypeMode::Requested), "requested");
    EXPECT_EQ(to_string(RequestedWeldType::L), "l");
    EXPECT_EQ(to_string(HumanCheckpointPolicy::Required), "required");
    EXPECT_EQ(
        to_string(static_cast<ApplicationSubmissionKind>(99)), "unknown");
    EXPECT_EQ(to_string(static_cast<WeldTypeMode>(99)), "unknown");
    EXPECT_EQ(to_string(static_cast<RequestedWeldType>(99)), "unknown");
    EXPECT_EQ(
        to_string(static_cast<HumanCheckpointPolicy>(99)), "unknown");
}

TEST(ApplicationSubmissionTest, CopyAndMovePreserveValidatedValues) {
    auto original = guidance_spec(
        WeldTypeMode::Requested, RequestedWeldType::Straight);
    const auto copied = original;
    auto moved = std::move(original);
    ASSERT_NE(copied.guidance(), nullptr);
    ASSERT_NE(original.guidance(), nullptr);
    ASSERT_NE(moved.guidance(), nullptr);
    EXPECT_EQ(copied, moved);
    EXPECT_EQ(original, moved);
    EXPECT_EQ(
        moved.guidance()->human_checkpoint(),
        HumanCheckpointPolicy::Required);
}

TEST(ApplicationSubmissionTest, CrossApplicationAndScenePairingsFailClosed) {
    const auto inspection = inspection_spec(true, false);
    const auto guidance = guidance_spec(WeldTypeMode::Auto, std::nullopt);
    EXPECT_TRUE(inspection.validate_for(
        IndustrialApplication::WeldInspection, ScenePhase::PostWeld));
    EXPECT_FALSE(inspection.validate_for(
        IndustrialApplication::WeldingGuidance, ScenePhase::PreWeld));
    EXPECT_TRUE(guidance.validate_for(
        IndustrialApplication::WeldingGuidance, ScenePhase::PreWeld));
    EXPECT_FALSE(guidance.validate_for(
        IndustrialApplication::WeldInspection, ScenePhase::PostWeld));
    EXPECT_FALSE(inspection.validate_for(
        IndustrialApplication::WeldInspection, ScenePhase::PreWeld));
}

}  // namespace
}  // namespace iaisf::application
