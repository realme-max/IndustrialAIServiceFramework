#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "iaisf/application/artifact_ref.hpp"

namespace iaisf::application {

class LocalArtifactCatalog;

struct OutputArtifactSpec final {
    std::string artifact_id;
    std::string kind;
    std::string media_type;
    std::optional<std::string> coordinate_frame;
    std::optional<std::string> unit;
    std::optional<std::uint64_t> point_count;
};

/** Registers files already produced below one controlled output root. */
class LocalOutputArtifactRegistrar final {
public:
    [[nodiscard]] static Result<std::unique_ptr<LocalOutputArtifactRegistrar>>
    make(const std::filesystem::path& output_root,
         std::shared_ptr<LocalArtifactCatalog> catalog = nullptr);

    LocalOutputArtifactRegistrar(const LocalOutputArtifactRegistrar&) = delete;
    LocalOutputArtifactRegistrar& operator=(const LocalOutputArtifactRegistrar&) = delete;

    [[nodiscard]] Result<ArtifactRef> register_file(
        const std::filesystem::path& file,
        const OutputArtifactSpec& spec) const;

private:
    explicit LocalOutputArtifactRegistrar(
        std::filesystem::path root, std::shared_ptr<LocalArtifactCatalog> catalog);
    std::filesystem::path root_;
    std::shared_ptr<LocalArtifactCatalog> catalog_;
};

}  // namespace iaisf::application
