#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "iaisf/application/artifact_ref.hpp"

namespace iaisf::application {

struct LocalArtifactCatalogEntry final {
    ArtifactRef artifact;
    std::filesystem::path canonical_path;
    std::string safe_filename;
};

struct LocalArtifactRegistration final {
    LocalArtifactCatalogEntry entry;
    bool created{false};
};

/**
 * Bounded, process-local index of artifacts that may be served over HTTP.
 * Paths are retained only in this private catalog; ArtifactRef remains
 * portable and contains no storage location.
 */
class LocalArtifactCatalog final {
public:
    [[nodiscard]] static Result<std::shared_ptr<LocalArtifactCatalog>> make(
        const std::filesystem::path& artifact_root,
        const std::filesystem::path& output_root,
        std::size_t max_entries = 4096U);

    LocalArtifactCatalog(const LocalArtifactCatalog&) = delete;
    LocalArtifactCatalog& operator=(const LocalArtifactCatalog&) = delete;

    [[nodiscard]] Result<LocalArtifactRegistration> register_artifact(
        ArtifactRef artifact,
        const std::filesystem::path& file,
        std::string safe_filename);

    [[nodiscard]] Result<std::optional<LocalArtifactCatalogEntry>> find(
        const std::string& artifact_id) const;

private:
    LocalArtifactCatalog(
        std::filesystem::path artifact_root,
        std::filesystem::path output_root,
        std::size_t max_entries);

    std::filesystem::path artifact_root_;
    std::filesystem::path output_root_;
    std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::map<std::string, LocalArtifactCatalogEntry> entries_;
};

}  // namespace iaisf::application
