#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "iaisf/application/application_artifacts.hpp"

namespace iaisf::application {

struct MaterializedPointCloud final {
    std::filesystem::path text_path;
    std::uint64_t point_count{};
};

/** Converts a verified binary artifact into one private, deterministic TXT. */
class PointCloudTxtMaterializer final {
public:
    [[nodiscard]] static Result<std::unique_ptr<PointCloudTxtMaterializer>>
    make(const std::filesystem::path& scratch_root);

    PointCloudTxtMaterializer(const PointCloudTxtMaterializer&) = delete;
    PointCloudTxtMaterializer& operator=(const PointCloudTxtMaterializer&) = delete;

    [[nodiscard]] Result<MaterializedPointCloud> materialize(
        const LocalArtifactResolver& resolver,
        const ArtifactRef& artifact,
        std::string_view job_id) const;

    [[nodiscard]] Result<void> cleanup(std::string_view job_id) const;

private:
    explicit PointCloudTxtMaterializer(std::filesystem::path root);
    std::filesystem::path root_;
};

}  // namespace iaisf::application
