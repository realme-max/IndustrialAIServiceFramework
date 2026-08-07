#include "iaisf/application/application_artifacts.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <new>
#include <nlohmann/json.hpp>
#include <string>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

class Sha256 final {
public:
    Sha256() noexcept : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
        0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(const unsigned char* data, const std::size_t length) noexcept {
        for (std::size_t index = 0U; index < length; ++index) {
            buffer_[buffer_size_++] = data[index];
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_.data());
                bit_count_ += 512U;
                buffer_size_ = 0U;
            }
        }
    }

    [[nodiscard]] std::string finish() {
        const auto original_bits = bit_count_ + buffer_size_ * 8U;
        buffer_[buffer_size_++] = 0x80U;
        while (buffer_size_ != 56U) {
            if (buffer_size_ == 64U) {
                transform(buffer_.data());
                buffer_size_ = 0U;
            }
            buffer_[buffer_size_++] = 0U;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[buffer_size_++] = static_cast<unsigned char>(
                (original_bits >> shift) & 0xffU);
        }
        transform(buffer_.data());
        static constexpr char hex[] = "0123456789abcdef";
        std::string output(64U, '0');
        for (std::size_t index = 0U; index < state_.size(); ++index) {
            output[index * 8U] = hex[(state_[index] >> 28U) & 0xfU];
            output[index * 8U + 1U] = hex[(state_[index] >> 24U) & 0xfU];
            output[index * 8U + 2U] = hex[(state_[index] >> 20U) & 0xfU];
            output[index * 8U + 3U] = hex[(state_[index] >> 16U) & 0xfU];
            output[index * 8U + 4U] = hex[(state_[index] >> 12U) & 0xfU];
            output[index * 8U + 5U] = hex[(state_[index] >> 8U) & 0xfU];
            output[index * 8U + 6U] = hex[(state_[index] >> 4U) & 0xfU];
            output[index * 8U + 7U] = hex[state_[index] & 0xfU];
        }
        return output;
    }

private:
    static constexpr std::uint32_t rotate_right(
        const std::uint32_t value, const std::uint32_t bits) noexcept {
        return (value >> bits) | (value << (32U - bits));
    }

    void transform(const unsigned char* block) noexcept {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcf,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
        std::uint32_t w[64]{};
        for (std::size_t i = 0U; i < 16U; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4U]) << 24U) |
                   (static_cast<std::uint32_t>(block[i * 4U + 1U]) << 16U) |
                   (static_cast<std::uint32_t>(block[i * 4U + 2U]) << 8U) |
                   static_cast<std::uint32_t>(block[i * 4U + 3U]);
        }
        for (std::size_t i = 16U; i < 64U; ++i) {
            const auto s0 = rotate_right(w[i - 15U], 7U) ^
                            rotate_right(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
            const auto s1 = rotate_right(w[i - 2U], 17U) ^
                            rotate_right(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
            w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
        }
        auto a = state_[0]; auto b = state_[1]; auto c = state_[2]; auto d = state_[3];
        auto e = state_[4]; auto f = state_[5]; auto g = state_[6]; auto h = state_[7];
        for (std::size_t i = 0U; i < 64U; ++i) {
            const auto s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + ch + k[i] + w[i];
            const auto s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffer_size_{0U};
    std::uint64_t bit_count_{0U};
};

Result<std::string> sha256_file(const std::filesystem::path& path) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return Result<std::string>::failure(make_error(
                ErrorCode::IoError, "artifact file cannot be opened"));
        }
        Sha256 hash;
        std::array<unsigned char, 64U * 1024U> buffer{};
        while (input) {
            input.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                hash.update(buffer.data(), static_cast<std::size_t>(count));
            }
        }
        if (!input.eof()) {
            return Result<std::string>::failure(make_error(
                ErrorCode::IoError, "artifact file cannot be read"));
        }
        return Result<std::string>::success(hash.finish());
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted, "artifact hash allocation failed"));
    }
}

bool exact_manifest(const nlohmann::json& manifest, const ArtifactRef& artifact) {
    static const std::array<const char*, 8U> keys = {
        "artifact_id", "sha256", "size_bytes", "kind", "media_type",
        "coordinate_frame", "unit", "point_count"};
    if (!manifest.is_object() || manifest.size() != keys.size()) {
        return false;
    }
    for (const auto* key : keys) {
        if (!manifest.contains(key)) {
            return false;
        }
    }
    return manifest.at("artifact_id") == artifact.artifact_id &&
           manifest.at("sha256") == artifact.sha256 &&
           manifest.at("size_bytes") == artifact.size_bytes &&
           manifest.at("kind") == artifact.kind &&
           manifest.at("media_type") == artifact.media_type &&
           manifest.at("coordinate_frame") == artifact.coordinate_frame.value_or("") &&
           manifest.at("unit") == artifact.unit.value_or("") &&
           manifest.at("point_count") == artifact.point_count.value_or(0U);
}

Result<void> validate_xyz_f32le_contract(const ArtifactRef& artifact) {
    constexpr std::uint64_t kBytesPerPoint = 12U;
    if (artifact.media_type !=
        "application/vnd.iaisf.pointcloud.xyz-f32le") {
        return Result<void>::success();
    }
    if (!artifact.point_count.has_value()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "xyz-f32le artifact point count is required"));
    }
    if (*artifact.point_count >
        std::numeric_limits<std::uint64_t>::max() / kBytesPerPoint) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "xyz-f32le artifact point count overflows its byte size"));
    }
    if (artifact.size_bytes != *artifact.point_count * kBytesPerPoint) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "xyz-f32le artifact size does not match point count"));
    }
    return Result<void>::success();
}

}  // namespace

LocalArtifactResolver::LocalArtifactResolver(
    std::filesystem::path canonical_root)
    : root_(std::move(canonical_root)) {}

Result<std::unique_ptr<LocalArtifactResolver>> LocalArtifactResolver::make(
    const std::filesystem::path& artifact_root) {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(artifact_root, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_directory(status)) {
            return Result<std::unique_ptr<LocalArtifactResolver>>::failure(
                make_error(ErrorCode::InvalidArgument,
                           "artifact root must be a real directory"));
        }
        const auto canonical = std::filesystem::canonical(artifact_root, error);
        if (error) {
            return Result<std::unique_ptr<LocalArtifactResolver>>::failure(
                make_error(ErrorCode::IoError, "artifact root cannot be canonicalized"));
        }
        return Result<std::unique_ptr<LocalArtifactResolver>>::success(
            std::unique_ptr<LocalArtifactResolver>{
                new LocalArtifactResolver{canonical}});
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<LocalArtifactResolver>>::failure(
            make_error(ErrorCode::ResourceExhausted, "artifact resolver allocation failed"));
    }
}

Result<std::filesystem::path> LocalArtifactResolver::resolve(
    const ArtifactRef& artifact) const {
    const auto valid = validate_artifact_ref(artifact);
    if (!valid) {
        return Result<std::filesystem::path>::failure(valid.error());
    }
    const auto valid_xyz = validate_xyz_f32le_contract(artifact);
    if (!valid_xyz) {
        return Result<std::filesystem::path>::failure(valid_xyz.error());
    }
    try {
        const auto directory = root_ / "inputs" / artifact.artifact_id;
        const auto path = directory / "pointcloud.xyzf32le";
        const auto manifest_path = directory / "artifact.json";
        std::error_code error;
        const auto directory_status = std::filesystem::symlink_status(directory, error);
        if (error || std::filesystem::is_symlink(directory_status) ||
            !std::filesystem::is_directory(directory_status)) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::NotFound, "artifact directory is unavailable"));
        }
        const auto file_status = std::filesystem::symlink_status(path, error);
        if (error || std::filesystem::is_symlink(file_status) ||
            !std::filesystem::is_regular_file(file_status)) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::NotFound, "artifact file is unavailable"));
        }
        const auto manifest_status = std::filesystem::symlink_status(manifest_path, error);
        if (error || std::filesystem::is_symlink(manifest_status) ||
            !std::filesystem::is_regular_file(manifest_status)) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::NotFound, "artifact manifest is unavailable"));
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error || size != artifact.size_bytes) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::InvalidArgument, "artifact size does not match manifest"));
        }
        std::ifstream manifest_stream(manifest_path);
        if (!manifest_stream) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::NotFound, "artifact manifest is unavailable"));
        }
        const auto manifest = nlohmann::json::parse(manifest_stream);
        if (!exact_manifest(manifest, artifact)) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::InvalidArgument, "artifact manifest does not match"));
        }
        const auto digest = sha256_file(path);
        if (!digest || digest.value() != artifact.sha256) {
            return Result<std::filesystem::path>::failure(
                make_error(ErrorCode::InvalidArgument, "artifact sha256 does not match"));
        }
        return Result<std::filesystem::path>::success(path);
    } catch (const nlohmann::json::exception&) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::InvalidArgument, "artifact manifest is invalid"));
    } catch (const std::bad_alloc&) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::ResourceExhausted, "artifact resolver allocation failed"));
    } catch (const std::filesystem::filesystem_error&) {
        return Result<std::filesystem::path>::failure(
            make_error(ErrorCode::IoError, "artifact filesystem operation failed"));
    }
}

}  // namespace iaisf::application
