#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <string>

#include "iaisf/application/artifact_ref.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] ArtifactRef valid_artifact() {
    return ArtifactRef{
        "art_cloud-001.v1",
        std::string(kSha256HexBytes, 'a'),
        1024U,
        "point_cloud",
        "application/vnd.iaisf.pointcloud.xyz-f32le",
        std::string{"camera"},
        std::string{"mm"},
        64U,
    };
}

TEST(ArtifactRefTest, AcceptsACompleteValidValue) {
    EXPECT_TRUE(validate_artifact_ref(valid_artifact()));
}

TEST(ArtifactRefTest, AcceptsAbsentPointCloudMetadata) {
    auto artifact = valid_artifact();
    artifact.coordinate_frame.reset();
    artifact.unit.reset();
    artifact.point_count.reset();
    EXPECT_TRUE(validate_artifact_ref(artifact));
}

TEST(ArtifactRefTest, ArtifactIdUsesStrictAsciiIdentifierSyntax) {
    auto artifact = valid_artifact();
    for (const std::string& rejected : std::array<std::string, 10U>{
             "", ".hidden", "../secret", "folder/file", R"(C:\secret)",
             R"(\\server\share)", "https://example.test/a", "a:b", "a b",
             std::string{"a\0b", 3U}}) {
        artifact.artifact_id = rejected;
        EXPECT_FALSE(validate_artifact_ref(artifact));
    }
}

TEST(ArtifactRefTest, ArtifactIdHonoursExactByteLimit) {
    auto artifact = valid_artifact();
    artifact.artifact_id = std::string(kMaxArtifactIdBytes, 'a');
    EXPECT_TRUE(validate_artifact_ref(artifact));
    artifact.artifact_id.push_back('a');
    EXPECT_FALSE(validate_artifact_ref(artifact));
}

TEST(ArtifactRefTest, Sha256RequiresLowercaseHexAndExactLength) {
    auto artifact = valid_artifact();
    artifact.sha256 = std::string(kSha256HexBytes - 1U, 'a');
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.sha256 = std::string(kSha256HexBytes + 1U, 'a');
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.sha256 = std::string(kSha256HexBytes, 'A');
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.sha256 = std::string(kSha256HexBytes, 'g');
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.sha256 = std::string(kSha256HexBytes, '0');
    EXPECT_TRUE(validate_artifact_ref(artifact));
}

TEST(ArtifactRefTest, SizeHonoursZeroMaximumAndOverflowBoundaries) {
    auto artifact = valid_artifact();
    artifact.size_bytes = 0U;
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.size_bytes = kMaxArtifactSizeBytes;
    EXPECT_TRUE(validate_artifact_ref(artifact));
    artifact.size_bytes = kMaxArtifactSizeBytes + 1U;
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.size_bytes = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(validate_artifact_ref(artifact));
}

TEST(ArtifactRefTest, RequiredTextFieldsAreBoundedAndRejectControls) {
    auto artifact = valid_artifact();

    artifact.kind.clear();
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact = valid_artifact();
    artifact.kind = std::string(kMaxArtifactKindBytes, 'k');
    EXPECT_TRUE(validate_artifact_ref(artifact));
    artifact.kind.push_back('k');
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact = valid_artifact();
    artifact.kind = "point\ncloud";
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.kind = std::string{"point\0cloud", 11U};
    EXPECT_FALSE(validate_artifact_ref(artifact));

    artifact = valid_artifact();
    artifact.media_type.clear();
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact = valid_artifact();
    artifact.media_type = std::string(kMaxArtifactMediaTypeBytes, 'm');
    EXPECT_TRUE(validate_artifact_ref(artifact));
    artifact.media_type.push_back('m');
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact = valid_artifact();
    artifact.media_type = "text/plain\r";
    EXPECT_FALSE(validate_artifact_ref(artifact));
}

TEST(ArtifactRefTest, OptionalTextFieldsAreBoundedAndRejectControls) {
    auto artifact = valid_artifact();
    artifact.coordinate_frame = std::string(kMaxCoordinateFrameBytes, 'f');
    artifact.unit = std::string(kMaxArtifactUnitBytes, 'u');
    EXPECT_TRUE(validate_artifact_ref(artifact));

    artifact.coordinate_frame->push_back('f');
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact = valid_artifact();
    artifact.unit->push_back('u');
    while (artifact.unit->size() <= kMaxArtifactUnitBytes) {
        artifact.unit->push_back('u');
    }
    EXPECT_FALSE(validate_artifact_ref(artifact));

    artifact = valid_artifact();
    artifact.coordinate_frame = "camera\tframe";
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.coordinate_frame = std::string{};
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact = valid_artifact();
    artifact.unit = std::string{"m\0m", 3U};
    EXPECT_FALSE(validate_artifact_ref(artifact));
}

TEST(ArtifactRefTest, PointCountHonoursZeroMaximumAndOverflowBoundaries) {
    auto artifact = valid_artifact();
    artifact.point_count = 0U;
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.point_count = kMaxArtifactPointCount;
    EXPECT_TRUE(validate_artifact_ref(artifact));
    artifact.point_count = kMaxArtifactPointCount + 1U;
    EXPECT_FALSE(validate_artifact_ref(artifact));
    artifact.point_count = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(validate_artifact_ref(artifact));
}

TEST(ArtifactRefTest, ErrorsAreBoundedAndDoNotEchoInput) {
    auto artifact = valid_artifact();
    const std::string sensitive =
        R"(C:\private\token-secret\artifact.json)";
    artifact.artifact_id = sensitive;
    const auto result = validate_artifact_ref(artifact);
    ASSERT_FALSE(result);
    EXPECT_LE(result.error().message.size(), 128U);
    EXPECT_EQ(result.error().message.find(sensitive), std::string::npos);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(ArtifactRefTest, ValidationReturnsStructuredResultsWithoutIo) {
    EXPECT_TRUE(validate_artifact_ref(valid_artifact()));

    auto artifact = valid_artifact();
    artifact.size_bytes = 0U;
    const auto rejected = validate_artifact_ref(artifact);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace iaisf::application
