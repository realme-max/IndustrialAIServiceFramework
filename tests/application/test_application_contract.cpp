#include <gtest/gtest.h>

#include <array>
#include <string>

#include "iaisf/application/application_contract.hpp"

namespace iaisf::application {
namespace {

const char* artifact_json() {
    return R"({"artifact_id":"input-001","sha256":"0000000000000000000000000000000000000000000000000000000000000000","size_bytes":1440,"kind":"point_cloud","media_type":"application/vnd.iaisf.pointcloud.xyz-f32le","coordinate_frame":"camera","unit":"mm","point_count":120})";
}

std::string inspection_json(const std::string& outputs) {
    return std::string{"{"} +
           R"("schema_version":"1.0","input_artifacts":[)" +
           artifact_json() + R"(],"requested_outputs":)" + outputs + "}";
}

std::string guidance_json(
    const std::string& weld_type,
    const std::string& checkpoint = "required") {
    return std::string{"{"} +
           R"("schema_version":"1.0","input_artifacts":[)" +
           artifact_json() + R"(],"weld_type":)" + weld_type +
           R"(,"review_policy":{"human_checkpoint":")" + checkpoint +
           R"("}})";
}

TEST(ApplicationContractTest, ParsesInspectionAndCanonicalizesOutputOrder) {
    const auto segmentation = parse_weld_inspection_submit(
        inspection_json(R"(["segmentation"])").c_str());
    const auto geometry = parse_weld_inspection_submit(
        inspection_json(R"(["geometry"])").c_str());
    const auto first = parse_weld_inspection_submit(
        inspection_json(R"(["segmentation","geometry"])").c_str());
    const auto second = parse_weld_inspection_submit(
        inspection_json(R"(["geometry","segmentation"])").c_str());
    ASSERT_TRUE(segmentation);
    ASSERT_TRUE(geometry);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value().application, IndustrialApplication::WeldInspection);
    EXPECT_EQ(first.value().scene_phase, ScenePhase::PostWeld);
    EXPECT_EQ(first.value().submission, second.value().submission);
    EXPECT_EQ(first.value().input_artifact.point_count, 120U);
    EXPECT_NE(
        segmentation.value().submission, geometry.value().submission);
}

TEST(ApplicationContractTest, ParsesGuidanceAutoAndRequestedModes) {
    const auto automatic = parse_welding_guidance_submit(
        guidance_json(R"({"mode":"auto"})").c_str());
    const auto requested = parse_welding_guidance_submit(
        guidance_json(R"({"mode":"requested","requested":"corner"})").c_str());
    const auto l_type = parse_welding_guidance_submit(
        guidance_json(R"({"mode":"requested","requested":"l"})").c_str());
    const auto not_required = parse_welding_guidance_submit(
        guidance_json(R"({"mode":"requested","requested":"straight"})",
                      "not_required")
            .c_str());
    ASSERT_TRUE(automatic);
    ASSERT_TRUE(requested);
    ASSERT_TRUE(l_type);
    ASSERT_TRUE(not_required);
    EXPECT_EQ(automatic.value().scene_phase, ScenePhase::PreWeld);
    ASSERT_NE(requested.value().submission.guidance(), nullptr);
    EXPECT_EQ(
        requested.value().submission.guidance()->weld_type().requested_type(),
        RequestedWeldType::Corner);
    EXPECT_EQ(
        l_type.value().submission.guidance()->weld_type().requested_type(),
        RequestedWeldType::L);
    EXPECT_EQ(
        not_required.value().submission.guidance()->human_checkpoint(),
        HumanCheckpointPolicy::NotRequired);
}

TEST(ApplicationContractTest, RejectsUnknownAndDuplicateFields) {
    auto unknown = inspection_json(R"(["geometry"])" );
    unknown.insert(unknown.size() - 1U, R"(,"extra":1)");
    const auto duplicate =
        parse_weld_inspection_submit(
            R"({"schema_version":"1.0","schema_version":"1.0","input_artifacts":[],"requested_outputs":["geometry"]})");
    EXPECT_EQ(
        parse_weld_inspection_submit(unknown).error().category,
        ApplicationContractErrorCategory::InvalidRequest);
    EXPECT_EQ(
        duplicate.error().category,
        ApplicationContractErrorCategory::InvalidJson);

    auto artifact_duplicate = inspection_json(R"(["geometry"])");
    const std::string artifact_field = R"("artifact_id":"input-001")";
    const auto artifact_position = artifact_duplicate.find(artifact_field);
    ASSERT_NE(artifact_position, std::string::npos);
    artifact_duplicate.insert(
        artifact_position + artifact_field.size(),
        "," + artifact_field);
    EXPECT_EQ(
        parse_weld_inspection_submit(artifact_duplicate).error().category,
        ApplicationContractErrorCategory::InvalidJson);

    for (const auto& outputs : std::array<const char*, 4>{
             "[]", R"(["geometry","geometry"])",
             R"(["unknown"])", "true"}) {
        EXPECT_EQ(
            parse_weld_inspection_submit(inspection_json(outputs)).error().category,
            ApplicationContractErrorCategory::InvalidRequest);
    }

    auto weld_type_duplicate = guidance_json(R"({"mode":"auto"})");
    const std::string mode_field = R"("mode":"auto")";
    const auto mode_position = weld_type_duplicate.find(mode_field);
    ASSERT_NE(mode_position, std::string::npos);
    weld_type_duplicate.insert(mode_position + mode_field.size(), "," + mode_field);
    EXPECT_EQ(
        parse_welding_guidance_submit(weld_type_duplicate).error().category,
        ApplicationContractErrorCategory::InvalidJson);

    auto review_duplicate = guidance_json(R"({"mode":"auto"})");
    const std::string checkpoint_field = R"("human_checkpoint":"required")";
    const auto checkpoint_position = review_duplicate.find(checkpoint_field);
    ASSERT_NE(checkpoint_position, std::string::npos);
    review_duplicate.insert(
        checkpoint_position + checkpoint_field.size(),
        "," + checkpoint_field);
    EXPECT_EQ(
        parse_welding_guidance_submit(review_duplicate).error().category,
        ApplicationContractErrorCategory::InvalidJson);
}

TEST(ApplicationContractTest, RejectsMalformedAndWrongSchema) {
    EXPECT_EQ(
        parse_weld_inspection_submit("not-json").error().category,
        ApplicationContractErrorCategory::InvalidJson);
    auto wrong_version = inspection_json(R"(["geometry"])" );
    const auto position = wrong_version.find("1.0");
    wrong_version.replace(position, 3U, "2.0");
    EXPECT_EQ(
        parse_weld_inspection_submit(wrong_version).error().category,
        ApplicationContractErrorCategory::InvalidRequest);
}

TEST(ApplicationContractTest, EnforcesExactlyOneArtifactAndExactArtifactSchema) {
    const auto zero =
        R"({"schema_version":"1.0","input_artifacts":[],"requested_outputs":["geometry"]})";
    EXPECT_EQ(
        parse_weld_inspection_submit(zero).error().category,
        ApplicationContractErrorCategory::InvalidRequest);
    auto missing = inspection_json(R"(["geometry"])" );
    const auto point = missing.find(",\"point_count\":120");
    missing.erase(point, std::string{",\"point_count\":120"}.size());
    EXPECT_EQ(
        parse_weld_inspection_submit(missing).error().category,
        ApplicationContractErrorCategory::InvalidRequest);

    auto artifact_unknown = inspection_json(R"(["geometry"])" );
    const auto artifact_end = artifact_unknown.find(
        R"(,"point_count":120})");
    ASSERT_NE(artifact_end, std::string::npos);
    artifact_unknown.insert(artifact_end, R"(,"extra":true)");
    EXPECT_EQ(
        parse_weld_inspection_submit(artifact_unknown).error().category,
        ApplicationContractErrorCategory::InvalidRequest);

    for (const auto& field : std::array<const char*, 8>{
             R"("artifact_id":"input-001",)",
             R"("sha256":"0000000000000000000000000000000000000000000000000000000000000000",)",
             R"("size_bytes":1440,)",
             R"("kind":"point_cloud",)",
             R"("media_type":"application/vnd.iaisf.pointcloud.xyz-f32le",)",
             R"("coordinate_frame":"camera",)",
             R"("unit":"mm",)",
             R"(,"point_count":120)"}) {
        auto missing_field = inspection_json(R"(["geometry"])");
        const auto field_position = missing_field.find(field);
        ASSERT_NE(field_position, std::string::npos);
        missing_field.erase(field_position, std::string{field}.size());
        EXPECT_EQ(
            parse_weld_inspection_submit(missing_field).error().category,
            ApplicationContractErrorCategory::InvalidRequest);
    }

    auto two = inspection_json(R"(["geometry"])");
    const auto artifacts_end = two.find(R"(],"requested_outputs")");
    ASSERT_NE(artifacts_end, std::string::npos);
    two.insert(artifacts_end, "," + std::string{artifact_json()});
    EXPECT_EQ(
        parse_weld_inspection_submit(two).error().category,
        ApplicationContractErrorCategory::InvalidRequest);

    auto wrong_type = inspection_json(R"(["geometry"])");
    const auto point_count = wrong_type.find(R"("point_count":120)");
    ASSERT_NE(point_count, std::string::npos);
    wrong_type.replace(
        point_count,
        std::string{R"("point_count":120)"}.size(),
        R"("point_count":true)");
    EXPECT_EQ(
        parse_weld_inspection_submit(wrong_type).error().category,
        ApplicationContractErrorCategory::InvalidRequest);
}

TEST(ApplicationContractTest, EnforcesArtifactMetadataAndOverflowSafeSizeRelation) {
    auto wrong_kind = inspection_json(R"(["geometry"])" );
    const auto kind = wrong_kind.find("point_cloud");
    wrong_kind.replace(kind, 11U, "mesh");
    EXPECT_EQ(
        parse_weld_inspection_submit(wrong_kind).error().category,
        ApplicationContractErrorCategory::ValidationFailed);
    auto wrong_size = inspection_json(R"(["geometry"])" );
    const auto size = wrong_size.find("1440");
    wrong_size.replace(size, 4U, "1441");
    EXPECT_EQ(
        parse_weld_inspection_submit(wrong_size).error().category,
        ApplicationContractErrorCategory::ValidationFailed);

    const auto max_point_count = kMaxArtifactSizeBytes / 12U;
    auto maximum = inspection_json(R"(["geometry"])");
    const auto maximum_count_position = maximum.find("120");
    ASSERT_NE(maximum_count_position, std::string::npos);
    maximum.replace(
        maximum_count_position,
        std::string{"120"}.size(),
        std::to_string(max_point_count));
    const auto maximum_size_position = maximum.find("1440");
    ASSERT_NE(maximum_size_position, std::string::npos);
    maximum.replace(
        maximum_size_position,
        std::string{"1440"}.size(),
        std::to_string(max_point_count * 12U));
    EXPECT_TRUE(parse_weld_inspection_submit(maximum));

    auto overflow = inspection_json(R"(["geometry"])");
    const auto overflow_count_position = overflow.find("120");
    ASSERT_NE(overflow_count_position, std::string::npos);
    overflow.replace(
        overflow_count_position,
        std::string{"120"}.size(),
        "18446744073709551615");
    const auto overflow_size_position = overflow.find("1440");
    ASSERT_NE(overflow_size_position, std::string::npos);
    overflow.replace(
        overflow_size_position,
        std::string{"1440"}.size(),
        "0");
    EXPECT_EQ(
        parse_weld_inspection_submit(overflow).error().category,
        ApplicationContractErrorCategory::ValidationFailed);

    for (const auto& replacement : std::array<const char*, 3>{
             "-1", "1.5", "true"}) {
        auto invalid_number = inspection_json(R"(["geometry"])");
        const auto number_position = invalid_number.find(
            R"("point_count":120)");
        ASSERT_NE(number_position, std::string::npos);
        invalid_number.replace(
            number_position,
            std::string{R"("point_count":120)"}.size(),
            std::string{"\"point_count\":"} + replacement);
        EXPECT_EQ(
            parse_weld_inspection_submit(invalid_number).error().category,
            ApplicationContractErrorCategory::InvalidRequest);
    }

    auto invalid_id = inspection_json(R"(["geometry"])");
    const auto id_position = invalid_id.find("input-001");
    ASSERT_NE(id_position, std::string::npos);
    invalid_id.replace(id_position, std::string{"input-001"}.size(), "../input");
    EXPECT_EQ(
        parse_weld_inspection_submit(invalid_id).error().category,
        ApplicationContractErrorCategory::ValidationFailed);

    auto invalid_sha = inspection_json(R"(["geometry"])");
    const auto sha_position = invalid_sha.find(std::string(64U, '0'));
    ASSERT_NE(sha_position, std::string::npos);
    invalid_sha.replace(sha_position, 64U, std::string(64U, 'A'));
    EXPECT_EQ(
        parse_weld_inspection_submit(invalid_sha).error().category,
        ApplicationContractErrorCategory::ValidationFailed);

    auto invalid_media_type = inspection_json(R"(["geometry"])");
    const auto media_position = invalid_media_type.find(
        "application/vnd.iaisf.pointcloud.xyz-f32le");
    ASSERT_NE(media_position, std::string::npos);
    invalid_media_type.replace(
        media_position,
        std::string{"application/vnd.iaisf.pointcloud.xyz-f32le"}.size(),
        "text/plain");
    EXPECT_EQ(
        parse_weld_inspection_submit(invalid_media_type).error().category,
        ApplicationContractErrorCategory::ValidationFailed);

    auto invalid_unit = inspection_json(R"(["geometry"])");
    const auto unit_position = invalid_unit.find(R"("unit":"mm")");
    ASSERT_NE(unit_position, std::string::npos);
    invalid_unit.replace(
        unit_position,
        std::string{R"("unit":"mm")"}.size(),
        R"("unit":"m")");
    EXPECT_EQ(
        parse_weld_inspection_submit(invalid_unit).error().category,
        ApplicationContractErrorCategory::ValidationFailed);

    auto invalid_frame = inspection_json(R"(["geometry"])");
    const auto frame_position = invalid_frame.find(
        R"("coordinate_frame":"camera")");
    ASSERT_NE(frame_position, std::string::npos);
    invalid_frame.replace(
        frame_position,
        std::string{R"("coordinate_frame":"camera")"}.size(),
        R"("coordinate_frame":"../frame")");
    EXPECT_EQ(
        parse_weld_inspection_submit(invalid_frame).error().category,
        ApplicationContractErrorCategory::ValidationFailed);
}

TEST(ApplicationContractTest, RejectsInvalidGuidanceAndDangerousFields) {
    EXPECT_EQ(
        parse_welding_guidance_submit(
            guidance_json(R"({"mode":"requested"})")).error().category,
        ApplicationContractErrorCategory::InvalidRequest);
    EXPECT_EQ(
        parse_welding_guidance_submit(
            guidance_json(R"({"mode":"requested","requested":"tee"})")).error().category,
        ApplicationContractErrorCategory::ValidationFailed);
    EXPECT_EQ(
        parse_welding_guidance_submit(
            guidance_json(R"({"mode":"auto","controller_url":"x"})")).error().category,
        ApplicationContractErrorCategory::InvalidRequest);
    for (const auto& checkpoint : {"unknown", "Not_Required", "REQUIRED", ""}) {
        EXPECT_EQ(
            parse_welding_guidance_submit(
                guidance_json(R"({"mode":"auto"})", checkpoint))
                .error()
                .category,
            ApplicationContractErrorCategory::ValidationFailed);
    }

    auto missing_checkpoint = guidance_json(R"({"mode":"requested","requested":"straight"})");
    const auto review_position = missing_checkpoint.find(
        R"(,"review_policy":{"human_checkpoint":"required"})");
    ASSERT_NE(review_position, std::string::npos);
    missing_checkpoint.erase(
        review_position,
        std::string{R"(,"review_policy":{"human_checkpoint":"required"})"}.size());
    EXPECT_EQ(
        parse_welding_guidance_submit(missing_checkpoint).error().category,
        ApplicationContractErrorCategory::InvalidRequest);

    EXPECT_EQ(
        parse_welding_guidance_submit(
            guidance_json(R"({"mode":"auto","requested":"straight"})"))
            .error()
            .category,
        ApplicationContractErrorCategory::InvalidRequest);

    auto review_unknown = guidance_json(R"({"mode":"auto"})");
    const auto review_extra_position = review_unknown.find(
        R"("human_checkpoint":"required")");
    ASSERT_NE(review_extra_position, std::string::npos);
    review_unknown.insert(
        review_extra_position + std::string{R"("human_checkpoint":"required")"}.size(),
        R"(,"extra":true)");
    EXPECT_EQ(
        parse_welding_guidance_submit(review_unknown).error().category,
        ApplicationContractErrorCategory::InvalidRequest);

    auto review_wrong_type = guidance_json(R"({"mode":"auto"})");
    const std::string review_value = R"("required")";
    const auto review_type_position = review_wrong_type.find(review_value);
    ASSERT_NE(review_type_position, std::string::npos);
    review_wrong_type.replace(review_type_position, review_value.size(), "true");
    EXPECT_EQ(
        parse_welding_guidance_submit(review_wrong_type).error().category,
        ApplicationContractErrorCategory::InvalidRequest);

    for (const auto& field : std::array<const char*, 10>{
             "controller_url", "send_url", "execute_robot", "joint_values",
             "allow_auto_execute", "shell", "ssh", "remote-command",
             "remote_command", "remote_command_url"}) {
        auto dangerous = guidance_json(R"({"mode":"auto"})");
        dangerous.insert(
            dangerous.size() - 1U,
            std::string{",\""} + field + R"(":true)");
        EXPECT_EQ(
            parse_welding_guidance_submit(dangerous).error().category,
            ApplicationContractErrorCategory::InvalidRequest);
    }

    auto nested_unknown = guidance_json(R"({"mode":"auto","nested":{"x":1}})");
    EXPECT_EQ(
        parse_welding_guidance_submit(nested_unknown).error().category,
        ApplicationContractErrorCategory::InvalidRequest);
}

TEST(ApplicationContractTest, RejectsPayloadOverConfiguredLimitWithoutEchoingBody) {
    std::string body(4097U, 'x');
    const auto result = parse_weld_inspection_submit(body);
    ASSERT_FALSE(result);
    EXPECT_EQ(
        result.error().category,
        ApplicationContractErrorCategory::PayloadTooLarge);
    EXPECT_EQ(
        result.error().message.find(std::string(32U, 'x')),
        std::string::npos);
}

}  // namespace
}  // namespace iaisf::application
