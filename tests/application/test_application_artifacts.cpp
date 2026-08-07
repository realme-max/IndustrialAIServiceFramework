#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <array>
#include <string>

#include "iaisf/application/application_artifacts.hpp"

namespace iaisf::application {
namespace {

class TempDirectory final {
public:
    TempDirectory() {
        root_ = std::filesystem::temp_directory_path() / "iaisf_mvp_artifacts";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_, error);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    const std::filesystem::path& path() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

ArtifactRef artifact() {
    return ArtifactRef{
        "pc-1", "15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b",
        12U, "point_cloud", "application/vnd.iaisf.pointcloud.xyz-f32le",
        std::string{"workpiece"}, std::string{"mm"}, 1U};
}

TEST(LocalArtifactResolverTest, ResolvesAndVerifiesCanonicalArtifact) {
    TempDirectory temp;
    const auto directory = temp.path() / "inputs" / "pc-1";
    std::filesystem::create_directories(directory);
    { std::array<char, 12U> point{};
      std::ofstream output(directory / "pointcloud.xyzf32le", std::ios::binary);
      output.write(point.data(), static_cast<std::streamsize>(point.size())); }
    std::ofstream manifest(directory / "artifact.json");
    manifest << R"({"artifact_id":"pc-1","sha256":"15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b","size_bytes":12,"kind":"point_cloud","media_type":"application/vnd.iaisf.pointcloud.xyz-f32le","coordinate_frame":"workpiece","unit":"mm","point_count":1})";
    manifest.close();

    const auto resolver = LocalArtifactResolver::make(temp.path());
    ASSERT_TRUE(resolver);
    const auto resolved = resolver.value()->resolve(artifact());
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved.value(), directory / "pointcloud.xyzf32le");
}

TEST(LocalArtifactResolverTest, RejectsSizeAndManifestMismatch) {
    TempDirectory temp;
    const auto directory = temp.path() / "inputs" / "pc-1";
    std::filesystem::create_directories(directory);
    { std::array<char, 12U> point{};
      std::ofstream output(directory / "pointcloud.xyzf32le", std::ios::binary);
      output.write(point.data(), static_cast<std::streamsize>(point.size())); }
    std::ofstream manifest(directory / "artifact.json");
    manifest << R"({"artifact_id":"pc-1","sha256":"15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b","size_bytes":8,"kind":"point_cloud","media_type":"application/vnd.iaisf.pointcloud.xyz-f32le","coordinate_frame":"workpiece","unit":"mm","point_count":1})";
    manifest.close();
    auto resolver = LocalArtifactResolver::make(temp.path());
    ASSERT_TRUE(resolver);
    EXPECT_FALSE(resolver.value()->resolve(artifact()));
}

TEST(LocalArtifactResolverTest, RejectsMissingOrMismatchedPointCount) {
    TempDirectory temp;
    const auto directory = temp.path() / "inputs" / "pc-1";
    std::filesystem::create_directories(directory);
    { std::array<char, 12U> point{};
      std::ofstream output(directory / "pointcloud.xyzf32le", std::ios::binary);
      output.write(point.data(), static_cast<std::streamsize>(point.size())); }
    std::ofstream manifest(directory / "artifact.json");
    manifest << R"({"artifact_id":"pc-1","sha256":"15ec7bf0b50732b49f8228e07d24365338f9e3ab994b00af08e5a3bffe55fd8b","size_bytes":12,"kind":"point_cloud","media_type":"application/vnd.iaisf.pointcloud.xyz-f32le","coordinate_frame":"workpiece","unit":"mm"})";
    manifest.close();
    auto resolver = LocalArtifactResolver::make(temp.path());
    ASSERT_TRUE(resolver);
    auto missing_point_count = artifact();
    missing_point_count.point_count.reset();
    EXPECT_FALSE(resolver.value()->resolve(missing_point_count));

    auto mismatched = artifact();
    mismatched.point_count = 2U;
    EXPECT_FALSE(resolver.value()->resolve(mismatched));
}

}  // namespace
}  // namespace iaisf::application
