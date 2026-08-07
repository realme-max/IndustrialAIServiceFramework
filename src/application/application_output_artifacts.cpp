#include "iaisf/application/application_output_artifacts.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <new>

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

bool contained(const std::filesystem::path& root,
               const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative == "." || relative == "..") return false;
    const auto native = relative.native();
    return native.size() < 2U || native[0] != native[1] || native[0] != '.' ||
           (native.size() > 2U && native[2] != std::filesystem::path::preferred_separator);
}

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
    static std::uint32_t rotate_right(const std::uint32_t value,
                                      const std::uint32_t bits) noexcept {
        return (value >> bits) | (value << (32U - bits));
    }

    void transform(const unsigned char* block) noexcept {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcf,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xbf597fc7U,0xc6e00bf2U,0xc6e00bf2U,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,0x748f82eeU,
            0x983e5152U,0xa831c66dU,0xbf597fc7U,0xc6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU};
        // The constants are kept private to this source file; the public API
        // exposes only the resulting ArtifactRef.
        static constexpr std::uint32_t standard_k[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcf,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xbf597fc7U,0xc6e00bf2U,0xc6e00bf2U,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,0x6a09e667U,
            0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U,0xc67178f2U};
        static constexpr std::uint32_t fixed_k[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcf,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
        (void)standard_k;
        std::uint32_t w[64]{};
        for (std::size_t index = 0U; index < 16U; ++index) {
            w[index] = (static_cast<std::uint32_t>(block[index * 4U]) << 24U) |
                       (static_cast<std::uint32_t>(block[index * 4U + 1U]) << 16U) |
                       (static_cast<std::uint32_t>(block[index * 4U + 2U]) << 8U) |
                       static_cast<std::uint32_t>(block[index * 4U + 3U]);
        }
        for (std::size_t index = 16U; index < 64U; ++index) {
            const auto s0 = rotate_right(w[index - 15U], 7U) ^
                            rotate_right(w[index - 15U], 18U) ^ (w[index - 15U] >> 3U);
            const auto s1 = rotate_right(w[index - 2U], 17U) ^
                            rotate_right(w[index - 2U], 19U) ^ (w[index - 2U] >> 10U);
            w[index] = w[index - 16U] + s0 + w[index - 7U] + s1;
        }
        auto a = state_[0]; auto b = state_[1]; auto c = state_[2]; auto d = state_[3];
        auto e = state_[4]; auto f = state_[5]; auto g = state_[6]; auto h = state_[7];
        for (std::size_t index = 0U; index < 64U; ++index) {
            const auto s1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + ch + fixed_k[index] + w[index];
            const auto s0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
        (void)k;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffer_size_{0U};
    std::uint64_t bit_count_{0U};
};

Result<std::string> sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return failure<std::string>(ErrorCode::IoError,
                                             "output artifact cannot be opened");
    Sha256 hash;
    std::array<unsigned char, 64U * 1024U> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!input.eof()) return failure<std::string>(ErrorCode::IoError,
                                                  "output artifact cannot be read");
    return Result<std::string>::success(hash.finish());
}

}  // namespace

LocalOutputArtifactRegistrar::LocalOutputArtifactRegistrar(
    std::filesystem::path root)
    : root_(std::move(root)) {}

Result<std::unique_ptr<LocalOutputArtifactRegistrar>>
LocalOutputArtifactRegistrar::make(const std::filesystem::path& output_root) {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(output_root, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_directory(status)) {
            return failure<std::unique_ptr<LocalOutputArtifactRegistrar>>(
                ErrorCode::InvalidArgument,
                "output artifact root must be a real directory");
        }
        const auto canonical = std::filesystem::canonical(output_root, error);
        if (error) return failure<std::unique_ptr<LocalOutputArtifactRegistrar>>(
            ErrorCode::IoError, "output artifact root cannot be resolved");
        return Result<std::unique_ptr<LocalOutputArtifactRegistrar>>::success(
            std::unique_ptr<LocalOutputArtifactRegistrar>{
                new LocalOutputArtifactRegistrar{canonical}});
    } catch (const std::bad_alloc&) {
        return failure<std::unique_ptr<LocalOutputArtifactRegistrar>>(
            ErrorCode::ResourceExhausted, "output artifact registrar allocation failed");
    }
}

Result<ArtifactRef> LocalOutputArtifactRegistrar::register_file(
    const std::filesystem::path& file,
    const OutputArtifactSpec& spec) const {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(file, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return failure<ArtifactRef>(ErrorCode::InvalidArgument,
                                        "output artifact must be a regular file");
        }
        const auto canonical = std::filesystem::canonical(file, error);
        if (error || !contained(root_, canonical)) {
            return failure<ArtifactRef>(ErrorCode::InvalidArgument,
                                        "output artifact is outside controlled root");
        }
        const auto size = std::filesystem::file_size(canonical, error);
        if (error || size == 0U || size > kMaxArtifactSizeBytes) {
            return failure<ArtifactRef>(ErrorCode::InvalidArgument,
                                        "output artifact size is invalid");
        }
        ArtifactRef artifact{
            spec.artifact_id, {}, static_cast<std::uint64_t>(size), spec.kind,
            spec.media_type, spec.coordinate_frame, spec.unit, spec.point_count};
        if (artifact.media_type == kXyzMediaType &&
            (!artifact.point_count.has_value() ||
             *artifact.point_count > std::numeric_limits<std::uint64_t>::max() / kBytesPerPoint ||
             artifact.size_bytes != *artifact.point_count * kBytesPerPoint)) {
            return failure<ArtifactRef>(ErrorCode::InvalidArgument,
                                        "xyz-f32le output size does not match point count");
        }
        const auto digest = sha256_file(canonical);
        if (!digest) return Result<ArtifactRef>::failure(digest.error());
        artifact.sha256 = digest.value();
        const auto valid = validate_artifact_ref(artifact);
        if (!valid) return Result<ArtifactRef>::failure(valid.error());
        const auto manifest_path = canonical.string() + ".artifact.json";
        const auto manifest_status = std::filesystem::symlink_status(manifest_path, error);
        if (!error && std::filesystem::exists(manifest_status)) {
            return failure<ArtifactRef>(ErrorCode::InvalidState,
                                        "output artifact manifest already exists");
        }
        nlohmann::ordered_json manifest{
            {"artifact_id", artifact.artifact_id}, {"sha256", artifact.sha256},
            {"size_bytes", artifact.size_bytes}, {"kind", artifact.kind},
            {"media_type", artifact.media_type}};
        if (artifact.coordinate_frame) manifest["coordinate_frame"] = *artifact.coordinate_frame;
        if (artifact.unit) manifest["unit"] = *artifact.unit;
        if (artifact.point_count) manifest["point_count"] = *artifact.point_count;
        const auto temporary = manifest_path + ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return failure<ArtifactRef>(ErrorCode::IoError,
                                                 "output artifact manifest cannot be created");
        output << manifest.dump() << '\n';
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return failure<ArtifactRef>(ErrorCode::IoError,
                                        "output artifact manifest cannot be written");
        }
        std::filesystem::rename(temporary, manifest_path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return failure<ArtifactRef>(ErrorCode::IoError,
                                        "output artifact manifest cannot be committed");
        }
        return Result<ArtifactRef>::success(std::move(artifact));
    } catch (const std::bad_alloc&) {
        return failure<ArtifactRef>(ErrorCode::ResourceExhausted,
                                    "output artifact registration allocation failed");
    } catch (const nlohmann::json::exception&) {
        return failure<ArtifactRef>(ErrorCode::InternalError,
                                    "output artifact manifest serialization failed");
    } catch (const std::filesystem::filesystem_error&) {
        return failure<ArtifactRef>(ErrorCode::IoError,
                                    "output artifact filesystem operation failed");
    }
}

}  // namespace iaisf::application
