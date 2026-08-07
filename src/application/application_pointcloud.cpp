#include "iaisf/application/application_pointcloud.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <new>
#include <sstream>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

constexpr std::uint64_t kBytesPerPoint = 12U;
constexpr char kXyzMediaType[] =
    "application/vnd.iaisf.pointcloud.xyz-f32le";

template <typename T>
Result<T> failure(const ErrorCode code, const char* message) {
    return Result<T>::failure(make_error(code, message));
}

bool valid_job_id(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxArtifactIdBytes) return false;
    const auto first = static_cast<unsigned char>(value.front());
    const bool first_ok = (first >= 'a' && first <= 'z') ||
                          (first >= 'A' && first <= 'Z') ||
                          (first >= '0' && first <= '9');
    if (!first_ok) return false;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool alnum = (byte >= 'a' && byte <= 'z') ||
                           (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9');
        if (!alnum && character != '-' && character != '_' && character != '.') {
            return false;
        }
    }
    return true;
}

bool contained(const std::filesystem::path& root,
               const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative == "." || relative == "..") return false;
    const auto native = relative.native();
    return native.size() < 2U || native[0] != native[1] || native[0] != '.' ||
           (native.size() > 2U && native[2] != std::filesystem::path::preferred_separator);
}

Result<void> validate_binary_contract(const ArtifactRef& artifact) {
    if (artifact.media_type != kXyzMediaType || !artifact.point_count.has_value()) {
        return failure<void>(ErrorCode::InvalidArgument,
                             "xyz-f32le artifact metadata is incomplete");
    }
    if (*artifact.point_count >
        std::numeric_limits<std::uint64_t>::max() / kBytesPerPoint ||
        artifact.size_bytes != *artifact.point_count * kBytesPerPoint) {
        return failure<void>(ErrorCode::InvalidArgument,
                             "xyz-f32le artifact size does not match point count");
    }
    return Result<void>::success();
}

float decode_float(const unsigned char* bytes) noexcept {
    const std::uint32_t bits = static_cast<std::uint32_t>(bytes[0]) |
                               (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                               (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                               (static_cast<std::uint32_t>(bytes[3]) << 24U);
    float value{};
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

PointCloudTxtMaterializer::PointCloudTxtMaterializer(
    std::filesystem::path root)
    : root_(std::move(root)) {}

Result<std::unique_ptr<PointCloudTxtMaterializer>>
PointCloudTxtMaterializer::make(const std::filesystem::path& scratch_root) {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(scratch_root, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_directory(status)) {
            return failure<std::unique_ptr<PointCloudTxtMaterializer>>(
                ErrorCode::InvalidArgument,
                "point cloud scratch root must be a real directory");
        }
        const auto canonical = std::filesystem::canonical(scratch_root, error);
        if (error) {
            return failure<std::unique_ptr<PointCloudTxtMaterializer>>(
                ErrorCode::IoError, "point cloud scratch root cannot be resolved");
        }
        return Result<std::unique_ptr<PointCloudTxtMaterializer>>::success(
            std::unique_ptr<PointCloudTxtMaterializer>{
                new PointCloudTxtMaterializer{canonical}});
    } catch (const std::bad_alloc&) {
        return failure<std::unique_ptr<PointCloudTxtMaterializer>>(
            ErrorCode::ResourceExhausted, "point cloud materializer allocation failed");
    }
}

Result<MaterializedPointCloud> PointCloudTxtMaterializer::materialize(
    const LocalArtifactResolver& resolver,
    const ArtifactRef& artifact,
    const std::string_view job_id) const {
    const auto valid_artifact = validate_artifact_ref(artifact);
    if (!valid_artifact) {
        return Result<MaterializedPointCloud>::failure(valid_artifact.error());
    }
    const auto valid_binary = validate_binary_contract(artifact);
    if (!valid_binary) {
        return Result<MaterializedPointCloud>::failure(valid_binary.error());
    }
    if (!valid_job_id(job_id)) {
        return failure<MaterializedPointCloud>(ErrorCode::InvalidArgument,
                                               "materialization job id is invalid");
    }
    const auto resolved = resolver.resolve(artifact);
    if (!resolved) {
        return Result<MaterializedPointCloud>::failure(resolved.error());
    }
    try {
        std::error_code error;
        const auto input_path = std::filesystem::canonical(resolved.value(), error);
        if (error) {
            return failure<MaterializedPointCloud>(
                ErrorCode::NotFound, "verified point cloud input is unavailable");
        }
        const auto size = std::filesystem::file_size(input_path, error);
        if (error || size != artifact.size_bytes) {
            return failure<MaterializedPointCloud>(
                ErrorCode::InvalidArgument, "verified point cloud size changed");
        }
        const auto job_directory = root_ / "jobs" / std::string(job_id) / "input";
        const auto text_path = job_directory / "pointcloud.txt";
        std::filesystem::create_directories(job_directory, error);
        if (error || !contained(root_, std::filesystem::weakly_canonical(
                                    job_directory, error)) || error) {
            return failure<MaterializedPointCloud>(
                ErrorCode::InvalidArgument, "materialization path escapes scratch root");
        }
        const auto text_status = std::filesystem::symlink_status(text_path, error);
        if (!error && std::filesystem::exists(text_status)) {
            return failure<MaterializedPointCloud>(
                ErrorCode::InvalidState, "materialized point cloud already exists");
        }
        const auto temporary = text_path.string() + ".tmp";
        std::ifstream input(input_path, std::ios::binary);
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!input || !output) {
            return failure<MaterializedPointCloud>(
                ErrorCode::IoError, "point cloud materialization file open failed");
        }
        output.imbue(std::locale::classic());
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << std::setprecision(std::numeric_limits<float>::max_digits10);
        std::array<unsigned char, kBytesPerPoint> bytes{};
        for (std::uint64_t index = 0U; index < *artifact.point_count; ++index) {
            input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
                std::filesystem::remove(temporary, error);
                return failure<MaterializedPointCloud>(
                    ErrorCode::IoError, "point cloud binary input is truncated");
            }
            const float x = decode_float(bytes.data());
            const float y = decode_float(bytes.data() + 4U);
            const float z = decode_float(bytes.data() + 8U);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                std::filesystem::remove(temporary, error);
                return failure<MaterializedPointCloud>(
                    ErrorCode::InvalidArgument,
                    "point cloud contains a non-finite coordinate");
            }
            line.str({});
            line.clear();
            line << x << ' ' << y << ' ' << z << '\n';
            output << line.str();
            if (!output) {
                std::filesystem::remove(temporary, error);
                return failure<MaterializedPointCloud>(
                    ErrorCode::IoError, "point cloud text output failed");
            }
        }
        output.flush();
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return failure<MaterializedPointCloud>(
                ErrorCode::IoError, "point cloud text output failed");
        }
        std::filesystem::rename(temporary, text_path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return failure<MaterializedPointCloud>(
                ErrorCode::IoError, "point cloud text commit failed");
        }
        return Result<MaterializedPointCloud>::success(
            MaterializedPointCloud{text_path, *artifact.point_count});
    } catch (const std::bad_alloc&) {
        return failure<MaterializedPointCloud>(
            ErrorCode::ResourceExhausted, "point cloud materializer allocation failed");
    } catch (const std::filesystem::filesystem_error&) {
        return failure<MaterializedPointCloud>(
            ErrorCode::IoError, "point cloud materializer filesystem operation failed");
    }
}

Result<void> PointCloudTxtMaterializer::cleanup(const std::string_view job_id) const {
    if (!valid_job_id(job_id)) {
        return failure<void>(ErrorCode::InvalidArgument,
                             "materialization job id is invalid");
    }
    try {
        const auto input_directory = root_ / "jobs" / std::string(job_id) / "input";
        std::error_code error;
        const auto canonical = std::filesystem::weakly_canonical(input_directory, error);
        if (error || !contained(root_, canonical)) {
            return failure<void>(ErrorCode::InvalidArgument,
                                 "materialization cleanup path is invalid");
        }
        std::filesystem::remove_all(input_directory, error);
        if (error) {
            return failure<void>(ErrorCode::IoError,
                                 "point cloud materialization cleanup failed");
        }
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return failure<void>(ErrorCode::ResourceExhausted,
                             "point cloud cleanup allocation failed");
    }
}

}  // namespace iaisf::application
