#pragma once

#include <filesystem>
#include <memory>

#include "iaisf/application/artifact_ref.hpp"
#include "iaisf/core/result.hpp"

namespace iaisf::application {

/**
 * Resolves already validated artifact ids inside one trusted local root.
 * The resolver performs no upload, download, deletion, overwrite or URL I/O.
 */
class LocalArtifactResolver final {
public:
    [[nodiscard]] static Result<std::unique_ptr<LocalArtifactResolver>> make(
        const std::filesystem::path& artifact_root);

    LocalArtifactResolver(const LocalArtifactResolver&) = delete;
    LocalArtifactResolver& operator=(const LocalArtifactResolver&) = delete;

    [[nodiscard]] Result<std::filesystem::path> resolve(
        const ArtifactRef& artifact) const;

private:
    explicit LocalArtifactResolver(std::filesystem::path canonical_root);

    std::filesystem::path root_;
};

}  // namespace iaisf::application
