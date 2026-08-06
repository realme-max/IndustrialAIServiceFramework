#include <gtest/gtest.h>

#include <array>
#include <string_view>

#include "iaisf/application/application_identity.hpp"

namespace iaisf::application {
namespace {

TEST(ApplicationIdentityTest, UsesStableApplicationStrings) {
    EXPECT_EQ(to_string(IndustrialApplication::WeldInspection),
              "weld_inspection");
    EXPECT_EQ(to_string(IndustrialApplication::WeldingGuidance),
              "welding_guidance");
    EXPECT_EQ(to_string(static_cast<IndustrialApplication>(-1)), "unknown");
}

TEST(ApplicationIdentityTest, UsesStableScenePhaseStrings) {
    EXPECT_EQ(to_string(ScenePhase::PostWeld), "post_weld");
    EXPECT_EQ(to_string(ScenePhase::PreWeld), "pre_weld");
    EXPECT_EQ(to_string(static_cast<ScenePhase>(-1)), "unknown");
}

TEST(ApplicationIdentityTest, StrictlyParsesApplications) {
    const auto inspection =
        parse_industrial_application("weld_inspection");
    ASSERT_TRUE(inspection);
    EXPECT_EQ(inspection.value(), IndustrialApplication::WeldInspection);

    const auto guidance =
        parse_industrial_application("welding_guidance");
    ASSERT_TRUE(guidance);
    EXPECT_EQ(guidance.value(), IndustrialApplication::WeldingGuidance);

    for (const std::string_view rejected :
         std::array<std::string_view, 5U>{
             "", "WeldInspection", "weld-inspection",
             "weld_inspection ", "unknown"}) {
        const auto result = parse_industrial_application(rejected);
        EXPECT_FALSE(result) << rejected;
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(ApplicationIdentityTest, StrictlyParsesScenePhases) {
    const auto post_weld = parse_scene_phase("post_weld");
    ASSERT_TRUE(post_weld);
    EXPECT_EQ(post_weld.value(), ScenePhase::PostWeld);

    const auto pre_weld = parse_scene_phase("pre_weld");
    ASSERT_TRUE(pre_weld);
    EXPECT_EQ(pre_weld.value(), ScenePhase::PreWeld);

    for (const std::string_view rejected :
         std::array<std::string_view, 5U>{
             "", "PostWeld", "post-weld", "pre_weld ", "unknown"}) {
        const auto result = parse_scene_phase(rejected);
        EXPECT_FALSE(result) << rejected;
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(ApplicationIdentityTest, OnlyAllowsTheTwoDomainPairings) {
    EXPECT_TRUE(validate_application_scene(
        IndustrialApplication::WeldInspection, ScenePhase::PostWeld));
    EXPECT_TRUE(validate_application_scene(
        IndustrialApplication::WeldingGuidance, ScenePhase::PreWeld));

    EXPECT_FALSE(validate_application_scene(
        IndustrialApplication::WeldInspection, ScenePhase::PreWeld));
    EXPECT_FALSE(validate_application_scene(
        IndustrialApplication::WeldingGuidance, ScenePhase::PostWeld));
}

TEST(ApplicationIdentityTest, InvalidEnumValuesFailClosed) {
    EXPECT_FALSE(validate_application_scene(
        static_cast<IndustrialApplication>(100), ScenePhase::PostWeld));
    EXPECT_FALSE(validate_application_scene(
        IndustrialApplication::WeldInspection,
        static_cast<ScenePhase>(100)));
}

}  // namespace
}  // namespace iaisf::application
