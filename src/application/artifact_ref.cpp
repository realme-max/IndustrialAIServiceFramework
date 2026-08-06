#include "iaisf/application/artifact_ref.hpp"

#include <string_view>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

[[nodiscard]] bool ascii_alnum(const unsigned char value) noexcept {
    return (value >= static_cast<unsigned char>('0') &&
            value <= static_cast<unsigned char>('9')) ||
           (value >= static_cast<unsigned char>('A') &&
            value <= static_cast<unsigned char>('Z')) ||
           (value >= static_cast<unsigned char>('a') &&
            value <= static_cast<unsigned char>('z'));
}

[[nodiscard]] bool valid_artifact_id(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxArtifactIdBytes ||
        !ascii_alnum(static_cast<unsigned char>(value.front()))) {
        return false;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (!ascii_alnum(byte) && character != '_' && character != '-' &&
            character != '.') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_sha256(const std::string_view value) noexcept {
    if (value.size() != kSha256HexBytes) {
        return false;
    }
    for (const char character : value) {
        const bool decimal = character >= '0' && character <= '9';
        const bool lower_hex = character >= 'a' && character <= 'f';
        if (!decimal && !lower_hex) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool bounded_text(
    const std::string_view value,
    const std::size_t maximum) noexcept {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == 0U || byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Result<void> invalid(const char* message) {
    return Result<void>::failure(make_error(ErrorCode::InvalidArgument, message));
}

}  // namespace

Result<void> validate_artifact_ref(const ArtifactRef& artifact) {
    if (!valid_artifact_id(artifact.artifact_id)) {
        return invalid("artifact id is invalid");
    }
    if (!valid_sha256(artifact.sha256)) {
        return invalid("artifact sha256 is invalid");
    }
    if (artifact.size_bytes == 0U ||
        artifact.size_bytes > kMaxArtifactSizeBytes) {
        return invalid("artifact size is outside the allowed range");
    }
    if (!bounded_text(artifact.kind, kMaxArtifactKindBytes)) {
        return invalid("artifact kind is invalid");
    }
    if (!bounded_text(artifact.media_type, kMaxArtifactMediaTypeBytes)) {
        return invalid("artifact media type is invalid");
    }
    if (artifact.coordinate_frame.has_value() &&
        !bounded_text(*artifact.coordinate_frame, kMaxCoordinateFrameBytes)) {
        return invalid("artifact coordinate frame is invalid");
    }
    if (artifact.unit.has_value() &&
        !bounded_text(*artifact.unit, kMaxArtifactUnitBytes)) {
        return invalid("artifact unit is invalid");
    }
    if (artifact.point_count.has_value() &&
        (*artifact.point_count == 0U ||
         *artifact.point_count > kMaxArtifactPointCount)) {
        return invalid("artifact point count is outside the allowed range");
    }
    return Result<void>::success();
}

}  // namespace iaisf::application
