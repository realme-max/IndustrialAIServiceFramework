#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "iaisf/core/result.hpp"

namespace iaisf::application {

inline constexpr std::size_t kMaxArtifactIdBytes = 128U;
inline constexpr std::size_t kSha256HexBytes = 64U;
inline constexpr std::uint64_t kMaxArtifactSizeBytes = 1ULL << 30U;
inline constexpr std::size_t kMaxArtifactKindBytes = 64U;
inline constexpr std::size_t kMaxArtifactMediaTypeBytes = 128U;
inline constexpr std::size_t kMaxCoordinateFrameBytes = 128U;
inline constexpr std::size_t kMaxArtifactUnitBytes = 32U;
inline constexpr std::uint64_t kMaxArtifactPointCount = 100'000'000ULL;

/**
 * Public, immutable-artifact description.
 *
 * This value deliberately contains no storage path, storage key, URL,
 * controller address, credential, or worker-local location.
 */
struct ArtifactRef {
    std::string artifact_id;
    std::string sha256;
    std::uint64_t size_bytes{};
    std::string kind;
    std::string media_type;
    std::optional<std::string> coordinate_frame;
    std::optional<std::string> unit;
    std::optional<std::uint64_t> point_count;
};

/** Performs bounded value validation only; it never accesses a filesystem. */
[[nodiscard]] Result<void> validate_artifact_ref(
    const ArtifactRef& artifact);

}  // namespace iaisf::application
