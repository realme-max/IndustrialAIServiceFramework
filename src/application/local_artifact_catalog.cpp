#include "iaisf/application/local_artifact_catalog.hpp"

#include <map>
#include <mutex>
#include <new>
#include <exception>
#include <system_error>
#include <string_view>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

template <typename T>
Result<T> failure(ErrorCode code, const char* message) {
    return Result<T>::failure(make_error(code, message));
}

bool contained(const std::filesystem::path& root,
               const std::filesystem::path& candidate) noexcept {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative == "." || relative == "..") {
        return false;
    }
    if (relative.is_absolute()) return false;
    for (const auto& component : relative) {
        if (component == "..") return false;
    }
    return true;
}

bool valid_artifact_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxArtifactIdBytes) return false;
    const auto first = static_cast<unsigned char>(value.front());
    const auto first_alnum = (first >= 'a' && first <= 'z') ||
                             (first >= 'A' && first <= 'Z') ||
                             (first >= '0' && first <= '9');
    if (!first_alnum) return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const auto alnum = (byte >= 'a' && byte <= 'z') ||
                           (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9');
        if (!alnum && character != '-' && character != '_' && character != '.') {
            return false;
        }
    }
    return true;
}

bool same_optional(const std::optional<std::string>& left,
                   const std::optional<std::string>& right) noexcept {
    return left == right;
}

bool same_artifact(const ArtifactRef& left, const ArtifactRef& right) noexcept {
    return left.artifact_id == right.artifact_id &&
           left.sha256 == right.sha256 && left.size_bytes == right.size_bytes &&
           left.kind == right.kind && left.media_type == right.media_type &&
           same_optional(left.coordinate_frame, right.coordinate_frame) &&
           same_optional(left.unit, right.unit) &&
           left.point_count == right.point_count;
}

bool valid_filename(const std::string& value) noexcept {
    if (value.empty() || value.size() > 128U) return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool alpha_num =
            (byte >= 'a' && byte <= 'z') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9');
        if (!alpha_num && character != '-' && character != '_' &&
            character != '.') {
            return false;
        }
    }
    return true;
}

Result<std::filesystem::path> canonical_root(
    const std::filesystem::path& root) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(root, error);
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
        return failure<std::filesystem::path>(
            ErrorCode::InvalidArgument,
            "artifact catalog root must be a real directory");
    }
    const auto canonical = std::filesystem::canonical(root, error);
    if (error) {
        return failure<std::filesystem::path>(
            ErrorCode::IoError, "artifact catalog root cannot be resolved");
    }
    return Result<std::filesystem::path>::success(canonical);
}

}  // namespace

LocalArtifactCatalog::LocalArtifactCatalog(
    std::filesystem::path artifact_root,
    std::filesystem::path output_root,
    const std::size_t max_entries)
    : artifact_root_(std::move(artifact_root)),
      output_root_(std::move(output_root)), max_entries_(max_entries) {}

Result<std::shared_ptr<LocalArtifactCatalog>> LocalArtifactCatalog::make(
    const std::filesystem::path& artifact_root,
    const std::filesystem::path& output_root,
    const std::size_t max_entries) {
    if (max_entries == 0U) {
        return failure<std::shared_ptr<LocalArtifactCatalog>>(
            ErrorCode::InvalidArgument,
            "artifact catalog capacity must be positive");
    }
    try {
        auto artifact = canonical_root(artifact_root);
        if (!artifact) return Result<std::shared_ptr<LocalArtifactCatalog>>::failure(
            std::move(artifact).error());
        auto output = canonical_root(output_root);
        if (!output) return Result<std::shared_ptr<LocalArtifactCatalog>>::failure(
            std::move(output).error());
        return Result<std::shared_ptr<LocalArtifactCatalog>>::success(
            std::shared_ptr<LocalArtifactCatalog>{new LocalArtifactCatalog{
                std::move(artifact).value(), std::move(output).value(),
                max_entries}});
    } catch (const std::bad_alloc&) {
        return failure<std::shared_ptr<LocalArtifactCatalog>>(
            ErrorCode::ResourceExhausted,
            "artifact catalog allocation failed");
    }
}

Result<LocalArtifactRegistration> LocalArtifactCatalog::register_artifact(
    ArtifactRef artifact,
    const std::filesystem::path& file,
    std::string safe_filename) {
    if (!valid_artifact_id(artifact.artifact_id)) {
        return failure<LocalArtifactRegistration>(
            ErrorCode::InvalidArgument, "artifact id is invalid");
    }
    const auto valid = validate_artifact_ref(artifact);
    if (!valid) return Result<LocalArtifactRegistration>::failure(valid.error());
    if (!valid_filename(safe_filename)) {
        return failure<LocalArtifactRegistration>(
            ErrorCode::InvalidArgument, "artifact download filename is invalid");
    }
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(file, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return failure<LocalArtifactRegistration>(
                ErrorCode::InvalidArgument,
                "catalog artifact must be a regular file");
        }
        const auto canonical = std::filesystem::canonical(file, error);
        if (error || (!contained(artifact_root_, canonical) &&
                      !contained(output_root_, canonical))) {
            return failure<LocalArtifactRegistration>(
                ErrorCode::InvalidArgument,
                "catalog artifact is outside controlled roots");
        }
        const auto size = std::filesystem::file_size(canonical, error);
        if (error || size != artifact.size_bytes || size == 0U ||
            size > kMaxArtifactSizeBytes) {
            return failure<LocalArtifactRegistration>(
                ErrorCode::InvalidArgument,
                "catalog artifact size does not match metadata");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(artifact.artifact_id);
        LocalArtifactCatalogEntry entry{
            std::move(artifact), canonical, std::move(safe_filename)};
        if (found != entries_.end()) {
            if (!same_artifact(found->second.artifact, entry.artifact) ||
                found->second.canonical_path != entry.canonical_path ||
                found->second.safe_filename != entry.safe_filename) {
                return failure<LocalArtifactRegistration>(
                    ErrorCode::InvalidState,
                    "artifact id is already registered with different content");
            }
            return Result<LocalArtifactRegistration>::success(
                LocalArtifactRegistration{found->second, false});
        }
        if (entries_.size() >= max_entries_) {
            return failure<LocalArtifactRegistration>(
                ErrorCode::ResourceExhausted,
                "artifact catalog capacity is exhausted");
        }
        const auto inserted = entries_.emplace(
            entry.artifact.artifact_id, entry);
        if (!inserted.second) {
            return failure<LocalArtifactRegistration>(
                ErrorCode::InvalidState, "artifact catalog registration failed");
        }
        return Result<LocalArtifactRegistration>::success(
            LocalArtifactRegistration{inserted.first->second, true});
    } catch (const std::bad_alloc&) {
        return failure<LocalArtifactRegistration>(
            ErrorCode::ResourceExhausted, "artifact catalog allocation failed");
    } catch (const std::filesystem::filesystem_error&) {
        return failure<LocalArtifactRegistration>(
            ErrorCode::IoError, "artifact catalog filesystem operation failed");
    } catch (const std::exception&) {
        return failure<LocalArtifactRegistration>(
            ErrorCode::InternalError, "artifact catalog registration failed");
    }
}

Result<std::optional<LocalArtifactCatalogEntry>> LocalArtifactCatalog::find(
    const std::string& artifact_id) const {
    if (!valid_artifact_id(artifact_id)) {
        return failure<std::optional<LocalArtifactCatalogEntry>>(
            ErrorCode::InvalidArgument, "artifact id is invalid");
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = entries_.find(artifact_id);
        if (found == entries_.end()) {
            return Result<std::optional<LocalArtifactCatalogEntry>>::success(
                std::nullopt);
        }
        return Result<std::optional<LocalArtifactCatalogEntry>>::success(
            found->second);
    } catch (const std::bad_alloc&) {
        return failure<std::optional<LocalArtifactCatalogEntry>>(
            ErrorCode::ResourceExhausted, "artifact catalog snapshot failed");
    } catch (const std::exception&) {
        return failure<std::optional<LocalArtifactCatalogEntry>>(
            ErrorCode::InternalError, "artifact catalog snapshot failed");
    }
}

}  // namespace iaisf::application
