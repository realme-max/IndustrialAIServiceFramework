#include "iaisf/application/artifact_http_api.hpp"

#include <array>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <new>
#include <set>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

using http::HttpResponse;
using http::HttpStatus;
using Json = nlohmann::ordered_json;

constexpr std::string_view kUploadPath{
    "/api/artifacts/v1/pointclouds"};
constexpr std::string_view kDownloadPrefix{
    "/api/artifacts/v1/files/"};
constexpr std::string_view kXyzMediaType{
    "application/vnd.iaisf.pointcloud.xyz-f32le"};

template <typename T>
Result<T> failure(ErrorCode code, const char* message) {
    return Result<T>::failure(make_error(code, message));
}

Result<HttpResponse> json_response(
    const HttpStatus status, Json body, const http::HttpLimits& limits) {
    try {
        auto serialized = body.dump();
        if (serialized.size() > limits.max_response_body_bytes()) {
            return failure<HttpResponse>(
                ErrorCode::ResourceExhausted,
                "artifact response exceeds the HTTP body limit");
        }
        HttpResponse response{status};
        auto header = response.set_header(
            "Content-Type", "application/json; charset=utf-8");
        if (!header) return Result<HttpResponse>::failure(header.error());
        response.set_body(std::move(serialized));
        return Result<HttpResponse>::success(std::move(response));
    } catch (const std::bad_alloc&) {
        return failure<HttpResponse>(
            ErrorCode::ResourceExhausted,
            "unable to allocate artifact response");
    } catch (...) {
        return failure<HttpResponse>(
            ErrorCode::InternalError,
            "unable to serialize artifact response");
    }
}

Result<HttpResponse> api_error(
    HttpStatus status, std::string_view code, std::string_view message,
    const http::HttpLimits& limits) {
    return json_response(status,
                         Json{{"error", {{"code", code}, {"message", message}}}},
                         limits);
}

bool content_type_is_text_plain(const std::optional<std::string>& value) {
    if (!value.has_value()) return false;
    const auto semicolon = value->find(';');
    return value->substr(0U, semicolon) == "text/plain";
}

bool valid_artifact_id(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxArtifactIdBytes) return false;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        const bool alpha_num = (byte >= 'a' && byte <= 'z') ||
                               (byte >= 'A' && byte <= 'Z') ||
                               (byte >= '0' && byte <= '9');
        if (!alpha_num && character != '_' && character != '-' &&
            character != '.') return false;
    }
    return true;
}

class Sha256 final {
public:
    Sha256() noexcept
        : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(const unsigned char* data, std::size_t length) noexcept {
        while (length != 0U) {
            const auto available = buffer_.size() - buffer_size_;
            const auto take = length < available ? length : available;
            std::memcpy(buffer_.data() + buffer_size_, data, take);
            data += take;
            length -= take;
            buffer_size_ += take;
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
        std::string result(64U, '0');
        for (std::size_t index = 0U; index < state_.size(); ++index) {
            for (std::size_t nibble = 0U; nibble < 8U; ++nibble) {
                const auto shift = static_cast<unsigned int>(28U - nibble * 4U);
                result[index * 8U + nibble] =
                    hex[(state_[index] >> shift) & 0xfU];
            }
        }
        return result;
    }

private:
    static std::uint32_t rotr(std::uint32_t value, std::uint32_t bits) noexcept {
        return (value >> bits) | (value << (32U - bits));
    }

    void transform(const unsigned char* block) noexcept {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcf,0xe9b5dba5U,0x3956c25bU,
            0x59f111f1U,0x923f82a4U,0xab1c5ed5U,0xd807aa98U,0x12835b01U,
            0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,
            0xc19bf174U,0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,
            0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,0x983e5152U,
            0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,
            0x06ca6351U,0x14292967U,0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,
            0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,
            0xd6990624U,0xf40e3585U,0x106aa070U,0x19a4c116U,0x1e376c08U,
            0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,
            0x682e6ff3U,0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
            0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
        std::uint32_t w[64]{};
        for (std::size_t i = 0U; i < 16U; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4U]) << 24U) |
                   (static_cast<std::uint32_t>(block[i * 4U + 1U]) << 16U) |
                   (static_cast<std::uint32_t>(block[i * 4U + 2U]) << 8U) |
                   static_cast<std::uint32_t>(block[i * 4U + 3U]);
        }
        for (std::size_t i = 16U; i < 64U; ++i) {
            const auto s0 = rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^
                            (w[i - 15U] >> 3U);
            const auto s1 = rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^
                            (w[i - 2U] >> 10U);
            w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
        }
        auto a = state_[0]; auto b = state_[1]; auto c = state_[2];
        auto d = state_[3]; auto e = state_[4]; auto f = state_[5];
        auto g = state_[6]; auto h = state_[7];
        for (std::size_t i = 0U; i < 64U; ++i) {
            const auto s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto t1 = h + s1 + ch + k[i] + w[i];
            const auto s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a;
            a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> buffer_{};
    std::size_t buffer_size_{0U};
    std::uint64_t bit_count_{0U};
};

Result<std::string> sha256_bytes(const std::vector<unsigned char>& bytes) {
    try {
        Sha256 hash;
        hash.update(bytes.data(), bytes.size());
        return Result<std::string>::success(hash.finish());
    } catch (const std::bad_alloc&) {
        return failure<std::string>(ErrorCode::ResourceExhausted,
                                    "artifact digest allocation failed");
    } catch (const std::exception&) {
        return failure<std::string>(ErrorCode::InternalError,
                                    "artifact digest failed");
    }
}

Result<std::string> sha256_text(const std::string& body) {
    try {
        Sha256 hash;
        hash.update(reinterpret_cast<const unsigned char*>(body.data()), body.size());
        return Result<std::string>::success(hash.finish());
    } catch (const std::bad_alloc&) {
        return failure<std::string>(ErrorCode::ResourceExhausted,
                                    "artifact digest allocation failed");
    } catch (const std::exception&) {
        return failure<std::string>(ErrorCode::InternalError,
                                    "artifact digest failed");
    }
}

Result<std::string> sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return failure<std::string>(ErrorCode::IoError,
                                            "artifact file cannot be opened");
    Sha256 hash;
    std::array<unsigned char, 64U * 1024U> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!input.eof()) return failure<std::string>(ErrorCode::IoError,
                                                  "artifact file cannot be read");
    return Result<std::string>::success(hash.finish());
}

enum class CoordinateStatus { Ok, Invalid, OutOfRange, Resource };

CoordinateStatus parse_coordinate(std::string_view token, float& output) {
    if (token.empty()) return CoordinateStatus::Invalid;
    const auto parsed = std::from_chars(
        token.data(), token.data() + token.size(), output,
        std::chars_format::general);
    if (parsed.ec == std::errc::result_out_of_range) {
        return CoordinateStatus::OutOfRange;
    }
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return CoordinateStatus::Invalid;
    }
    return std::isfinite(output) ? CoordinateStatus::Ok
                                 : CoordinateStatus::OutOfRange;
}

enum class XyzParseFailure { Malformed, OutOfRange, Resource };

struct ParsedXyz final {
    std::vector<unsigned char> bytes;
    std::uint64_t point_count{};
};

struct XyzParseOutcome final {
    std::optional<ParsedXyz> value;
    XyzParseFailure failure{XyzParseFailure::Malformed};
};

bool whitespace_only(std::string_view line) noexcept {
    for (const auto character : line) {
        if (character != ' ' && character != '\t' && character != '\r') {
            return false;
        }
    }
    return true;
}

XyzParseOutcome parse_xyz(std::string_view body) {
    try {
        std::vector<unsigned char> bytes;
        std::size_t point_count = 0U;
        std::size_t line_start = 0U;
        while (line_start <= body.size()) {
            auto line_end = body.find('\n', line_start);
            if (line_end == std::string_view::npos) line_end = body.size();
            auto line = body.substr(line_start, line_end - line_start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1U);
            if (whitespace_only(line)) {
                if (line_end == body.size()) break;
                line_start = line_end + 1U;
                continue;
            }
            std::size_t cursor = 0U;
            std::array<float, 3> coordinates{};
            for (std::size_t column = 0U; column < 3U; ++column) {
                while (cursor < line.size() &&
                       (line[cursor] == ' ' || line[cursor] == '\t')) ++cursor;
                const auto token_start = cursor;
                while (cursor < line.size() && line[cursor] != ' ' &&
                       line[cursor] != '\t') ++cursor;
                if (token_start == cursor) {
                    if (line.empty()) break;
                    return XyzParseOutcome{std::nullopt,
                                           XyzParseFailure::Malformed};
                }
                const auto status = parse_coordinate(
                    line.substr(token_start, cursor - token_start),
                    coordinates[column]);
                if (status == CoordinateStatus::Invalid) {
                    return XyzParseOutcome{std::nullopt,
                                           XyzParseFailure::Malformed};
                }
                if (status == CoordinateStatus::OutOfRange) {
                    return XyzParseOutcome{std::nullopt,
                                           XyzParseFailure::OutOfRange};
                }
                if (status == CoordinateStatus::Resource) {
                    return XyzParseOutcome{std::nullopt,
                                           XyzParseFailure::Resource};
                }
            }
            if (!line.empty()) {
                if (bytes.size() > kMaxArtifactSizeBytes - 12U) {
                    return XyzParseOutcome{std::nullopt,
                                           XyzParseFailure::Resource};
                }
                for (const auto coordinate : coordinates) {
                    std::uint32_t bits{};
                    std::memcpy(&bits, &coordinate, sizeof(bits));
                    bytes.push_back(static_cast<unsigned char>(bits & 0xffU));
                    bytes.push_back(static_cast<unsigned char>((bits >> 8U) & 0xffU));
                    bytes.push_back(static_cast<unsigned char>((bits >> 16U) & 0xffU));
                    bytes.push_back(static_cast<unsigned char>((bits >> 24U) & 0xffU));
                }
                ++point_count;
            }
            if (line_end == body.size()) break;
            line_start = line_end + 1U;
        }
        if (point_count == 0U) {
            return XyzParseOutcome{std::nullopt, XyzParseFailure::Malformed};
        }
        return XyzParseOutcome{ParsedXyz{std::move(bytes),
                                         static_cast<std::uint64_t>(point_count)},
                               XyzParseFailure::Malformed};
    } catch (const std::bad_alloc&) {
        return XyzParseOutcome{std::nullopt, XyzParseFailure::Resource};
    }
}

Result<void> write_manifest(
    const std::filesystem::path& manifest,
    const ArtifactRef& artifact) {
    try {
        Json value{{"artifact_id", artifact.artifact_id},
                   {"sha256", artifact.sha256},
                   {"size_bytes", artifact.size_bytes},
                   {"kind", artifact.kind},
                   {"media_type", artifact.media_type},
                   {"coordinate_frame", *artifact.coordinate_frame},
                   {"unit", *artifact.unit},
                   {"point_count", *artifact.point_count}};
        const auto temporary = manifest.string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return failure<void>(ErrorCode::IoError,
                                          "artifact manifest cannot be created");
        output << value.dump() << '\n';
        output.close();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return failure<void>(ErrorCode::IoError,
                                 "artifact manifest cannot be written");
        }
        std::error_code error;
        std::filesystem::rename(temporary, manifest, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return failure<void>(ErrorCode::IoError,
                                 "artifact manifest cannot be committed");
        }
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return failure<void>(ErrorCode::ResourceExhausted,
                             "artifact manifest allocation failed");
    }
}

Result<std::string> read_bounded_text_file(
    const std::filesystem::path& path, const std::size_t maximum_bytes) {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return failure<std::string>(ErrorCode::InvalidState,
                                        "artifact manifest is unavailable");
        }
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > maximum_bytes) {
            return failure<std::string>(ErrorCode::InvalidState,
                                        "artifact manifest is unavailable");
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) return failure<std::string>(ErrorCode::InvalidState,
                                                "artifact manifest is unavailable");
        std::string text(static_cast<std::size_t>(size), '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(text.size())) {
            return failure<std::string>(ErrorCode::InvalidState,
                                        "artifact manifest is unavailable");
        }
        return Result<std::string>::success(std::move(text));
    } catch (const std::bad_alloc&) {
        return failure<std::string>(ErrorCode::ResourceExhausted,
                                    "artifact manifest allocation failed");
    } catch (const std::exception&) {
        return failure<std::string>(ErrorCode::InvalidState,
                                    "artifact manifest is unavailable");
    }
}

Result<ArtifactRef> read_upload_manifest(
    const std::filesystem::path& manifest,
    const ArtifactRef& expected) {
    auto text = read_bounded_text_file(manifest, 16U * 1024U);
    if (!text) return Result<ArtifactRef>::failure(text.error());
    try {
        bool duplicate_key = false;
        std::set<std::string> keys;
        const auto callback = [&keys, &duplicate_key](
                                  int depth, Json::parse_event_t event,
                                  Json& parsed) {
            if (event == Json::parse_event_t::key) {
                if (depth < 0 || !parsed.is_string() ||
                    !keys.insert(parsed.get<std::string>()).second) {
                    duplicate_key = true;
                    return false;
                }
            }
            return true;
        };
        const auto value = Json::parse(text.value(), callback, true, false);
        if (duplicate_key || !value.is_object() || value.size() != 8U) {
            return failure<ArtifactRef>(ErrorCode::InvalidState,
                                        "artifact manifest conflicts with metadata");
        }
        static constexpr std::array<std::string_view, 8U> fields{
            "artifact_id", "sha256", "size_bytes", "kind", "media_type",
            "coordinate_frame", "unit", "point_count"};
        for (const auto field : fields) {
            if (!value.contains(field)) {
                return failure<ArtifactRef>(ErrorCode::InvalidState,
                                            "artifact manifest conflicts with metadata");
            }
        }
        if (!value.at("artifact_id").is_string() ||
            !value.at("sha256").is_string() ||
            !value.at("size_bytes").is_number_unsigned() ||
            !value.at("kind").is_string() ||
            !value.at("media_type").is_string() ||
            !value.at("coordinate_frame").is_string() ||
            !value.at("unit").is_string() ||
            !value.at("point_count").is_number_unsigned()) {
            return failure<ArtifactRef>(ErrorCode::InvalidState,
                                        "artifact manifest conflicts with metadata");
        }
        ArtifactRef actual{
            value.at("artifact_id").get<std::string>(),
            value.at("sha256").get<std::string>(),
            value.at("size_bytes").get<std::uint64_t>(),
            value.at("kind").get<std::string>(),
            value.at("media_type").get<std::string>(),
            value.at("coordinate_frame").get<std::string>(),
            value.at("unit").get<std::string>(),
            value.at("point_count").get<std::uint64_t>()};
        const auto valid = validate_artifact_ref(actual);
        if (!valid || actual.artifact_id != expected.artifact_id ||
            actual.sha256 != expected.sha256 ||
            actual.size_bytes != expected.size_bytes ||
            actual.kind != expected.kind ||
            actual.media_type != expected.media_type ||
            actual.coordinate_frame != expected.coordinate_frame ||
            actual.unit != expected.unit ||
            actual.point_count != expected.point_count) {
            return failure<ArtifactRef>(ErrorCode::InvalidState,
                                        "artifact manifest conflicts with metadata");
        }
        return Result<ArtifactRef>::success(std::move(actual));
    } catch (const std::bad_alloc&) {
        return failure<ArtifactRef>(ErrorCode::ResourceExhausted,
                                    "artifact manifest allocation failed");
    } catch (const std::exception&) {
        return failure<ArtifactRef>(ErrorCode::InvalidState,
                                    "artifact manifest conflicts with metadata");
    }
}

Result<std::filesystem::path> canonical_directory(
    const std::filesystem::path& root) {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(root, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_directory(status)) {
            return failure<std::filesystem::path>(
                ErrorCode::InvalidArgument, "artifact root must be a real directory");
        }
        const auto canonical = std::filesystem::canonical(root, error);
        if (error) return failure<std::filesystem::path>(
            ErrorCode::IoError, "artifact root cannot be resolved");
        return Result<std::filesystem::path>::success(canonical);
    } catch (const std::bad_alloc&) {
        return failure<std::filesystem::path>(ErrorCode::ResourceExhausted,
                                              "artifact root allocation failed");
    } catch (const std::exception&) {
        return failure<std::filesystem::path>(ErrorCode::IoError,
                                              "artifact root cannot be resolved");
    }
}

}  // namespace

ArtifactHttpApi::ArtifactHttpApi(
    std::filesystem::path artifact_root,
    std::shared_ptr<LocalArtifactCatalog> catalog,
    http::HttpLimits limits) noexcept
    : artifact_root_(std::move(artifact_root)), catalog_(std::move(catalog)),
      limits_(std::move(limits)) {}

Result<ArtifactHttpApi::Ptr> ArtifactHttpApi::create(
    std::filesystem::path artifact_root,
    std::shared_ptr<LocalArtifactCatalog> catalog,
    http::HttpLimits limits) {
    if (artifact_root.empty() || !catalog) {
        return failure<Ptr>(ErrorCode::InvalidArgument,
                            "artifact HTTP API options are incomplete");
    }
    try {
        auto canonical = canonical_directory(artifact_root);
        if (!canonical) return Result<Ptr>::failure(canonical.error());
        return Result<Ptr>::success(std::shared_ptr<ArtifactHttpApi>{
            new ArtifactHttpApi{std::move(canonical).value(), std::move(catalog),
                                std::move(limits)}});
    } catch (const std::bad_alloc&) {
        return failure<Ptr>(ErrorCode::ResourceExhausted,
                            "artifact HTTP API allocation failed");
    } catch (const std::exception&) {
        return failure<Ptr>(ErrorCode::InternalError,
                            "artifact HTTP API initialization failed");
    }
}

Result<void> ArtifactHttpApi::register_routes(http::HttpRouter& router) {
    try {
        const auto weak = weak_from_this();
        auto upload_route = router.add_route(
            "POST", std::string{kUploadPath},
            [weak](const http::HttpRequest& request) {
                const auto api = weak.lock();
                if (!api) return Result<HttpResponse>::success(
                    HttpResponse::error(HttpStatus::ServiceUnavailable, true));
                return api->upload(request);
            });
        if (!upload_route) return upload_route;
        return router.add_terminal_parameter_route(
            "GET", std::string{kDownloadPrefix},
            [weak](const http::HttpRequest& request, const std::string& id) {
                const auto api = weak.lock();
                if (!api) return Result<HttpResponse>::success(
                    HttpResponse::error(HttpStatus::ServiceUnavailable, true));
                return api->download(request, id);
            });
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "artifact HTTP route allocation failed"));
    } catch (const std::exception&) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "artifact HTTP route registration failed"));
    }
}

Result<HttpResponse> ArtifactHttpApi::upload(const http::HttpRequest& request) {
    if (!content_type_is_text_plain(request.header("content-type"))) {
        return api_error(HttpStatus::UnsupportedMediaType, "unsupported_media_type",
                         "content type must be text/plain", limits_);
    }
    if (request.body().size() > limits_.max_body_bytes()) {
        return api_error(HttpStatus::PayloadTooLarge, "payload_too_large",
                         "point cloud exceeds the configured request limit", limits_);
    }
    const auto parsed = parse_xyz(request.body());
    if (!parsed.value.has_value()) {
        const auto status = parsed.failure == XyzParseFailure::OutOfRange
                                ? HttpStatus::UnprocessableContent
                                : parsed.failure == XyzParseFailure::Resource
                                      ? HttpStatus::ServiceUnavailable
                                      : HttpStatus::BadRequest;
        return api_error(status,
                         parsed.failure == XyzParseFailure::Resource
                             ? "storage_failure" : "invalid_point_cloud",
                         parsed.failure == XyzParseFailure::Resource
                             ? "point cloud storage is unavailable"
                             : "point cloud input is invalid", limits_);
    }
    const auto& bytes = parsed.value->bytes;
    auto digest = sha256_bytes(bytes);
    if (!digest) return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                                  "artifact storage is unavailable", limits_);
    const auto point_count = parsed.value->point_count;
    ArtifactRef artifact{
        "pc_" + digest.value(), digest.value(),
        static_cast<std::uint64_t>(bytes.size()), "point_cloud",
        std::string{kXyzMediaType}, std::string{"camera"}, std::string{"mm"},
        point_count};
    const auto valid = validate_artifact_ref(artifact);
    if (!valid) return api_error(HttpStatus::UnprocessableContent, "invalid_point_cloud",
                                 "point cloud input is invalid", limits_);

    try {
        std::lock_guard<std::mutex> upload_lock(upload_mutex_);
        const auto directory = artifact_root_ / "inputs" / artifact.artifact_id;
        std::error_code error;
        const auto inputs = artifact_root_ / "inputs";
        const auto inputs_status = std::filesystem::symlink_status(inputs, error);
        if (error == std::errc::no_such_file_or_directory) error.clear();
        if (!error && std::filesystem::is_symlink(inputs_status)) {
            return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                             "artifact storage is unavailable", limits_);
        }
        if (!error && !std::filesystem::exists(inputs_status)) {
            std::filesystem::create_directory(inputs, error);
        }
        const auto verified_inputs = std::filesystem::symlink_status(inputs, error);
        if (error || std::filesystem::is_symlink(verified_inputs) ||
            !std::filesystem::is_directory(verified_inputs)) {
            return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                             "artifact storage is unavailable", limits_);
        }
        const auto directory_status = std::filesystem::symlink_status(directory, error);
        if (error == std::errc::no_such_file_or_directory) error.clear();
        if (!error && std::filesystem::is_symlink(directory_status)) {
            return api_error(HttpStatus::InternalServerError, "storage_conflict",
                             "artifact storage is inconsistent", limits_);
        }
        if (!error && !std::filesystem::exists(directory_status)) {
            std::filesystem::create_directory(directory, error);
        }
        if (error) return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                                    "artifact storage is unavailable", limits_);
        const auto canonical_directory_path =
            std::filesystem::canonical(directory, error);
        const auto canonical_root = std::filesystem::canonical(artifact_root_, error);
        if (error || canonical_directory_path.lexically_relative(canonical_root).empty()) {
            return api_error(HttpStatus::InternalServerError, "storage_conflict",
                             "artifact storage is inconsistent", limits_);
        }
        const auto file = directory / "pointcloud.xyzf32le";
        const auto manifest = directory / "artifact.json";
        bool existed = false;
        bool created_file = false;
        bool created_manifest = false;
        const auto cleanup_new_artifact = [&] {
            if (!created_file) return;
            std::error_code ignored;
            if (created_manifest) {
                std::filesystem::remove(manifest, ignored);
            }
            std::filesystem::remove(file, ignored);
            std::filesystem::remove(directory, ignored);
        };
        const auto status = std::filesystem::symlink_status(file, error);
        if (!error && std::filesystem::exists(status)) {
            existed = true;
            if (std::filesystem::is_symlink(status) ||
                !std::filesystem::is_regular_file(status) ||
                std::filesystem::file_size(file, error) != artifact.size_bytes || error) {
                return api_error(HttpStatus::InternalServerError, "storage_conflict",
                                 "artifact storage is inconsistent", limits_);
            }
            auto existing_digest = sha256_file(file);
            if (!existing_digest || existing_digest.value() != artifact.sha256) {
                return api_error(HttpStatus::InternalServerError, "storage_conflict",
                                 "artifact storage is inconsistent", limits_);
            }
        } else {
            const auto temporary = file.string() + ".tmp";
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                                          "artifact storage is unavailable", limits_);
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output) {
                std::filesystem::remove(temporary, error);
                return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                                 "artifact storage is unavailable", limits_);
            }
            std::filesystem::rename(temporary, file, error);
            if (error) {
                std::filesystem::remove(temporary, error);
                return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                                 "artifact storage is unavailable", limits_);
            }
            created_file = true;
        }
        const auto manifest_status = std::filesystem::symlink_status(manifest, error);
        if (!error && std::filesystem::exists(manifest_status) &&
            (std::filesystem::is_symlink(manifest_status) ||
             !std::filesystem::is_regular_file(manifest_status))) {
            cleanup_new_artifact();
            return api_error(HttpStatus::InternalServerError, "storage_conflict",
                             "artifact storage is inconsistent", limits_);
        }
        if (error || !std::filesystem::exists(manifest_status)) {
            auto written = write_manifest(manifest, artifact);
            if (!written) {
                cleanup_new_artifact();
                return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                                 "artifact storage is unavailable", limits_);
            }
            created_manifest = true;
        } else {
            const auto existing_manifest = read_upload_manifest(manifest, artifact);
            if (!existing_manifest) {
                cleanup_new_artifact();
                return api_error(HttpStatus::InternalServerError, "storage_conflict",
                                 "artifact storage is inconsistent", limits_);
            }
        }
        auto registered = catalog_->register_artifact(
            artifact, file, artifact.artifact_id + ".xyzf32le");
        if (!registered) {
            cleanup_new_artifact();
            return api_error(registered.error().code == ErrorCode::InvalidState
                                 ? HttpStatus::InternalServerError
                                 : HttpStatus::ServiceUnavailable,
                             registered.error().code == ErrorCode::InvalidState
                                 ? "storage_conflict" : "storage_failure",
                             registered.error().code == ErrorCode::InvalidState
                                 ? "artifact storage is inconsistent"
                                 : "artifact storage is unavailable", limits_);
        }
        Json body{{"schema_version", "1.0"},
                  {"artifact", Json{{"artifact_id", artifact.artifact_id},
                                     {"sha256", artifact.sha256},
                                     {"size_bytes", artifact.size_bytes},
                                     {"kind", artifact.kind},
                                     {"media_type", artifact.media_type},
                                     {"coordinate_frame", *artifact.coordinate_frame},
                                     {"unit", *artifact.unit},
                                     {"point_count", *artifact.point_count}}},
                  {"download_url", std::string{kDownloadPrefix} + artifact.artifact_id}};
        return json_response(existed || !registered.value().created
                                 ? HttpStatus::Ok : HttpStatus::Created,
                             std::move(body), limits_);
    } catch (const std::bad_alloc&) {
        return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                         "artifact storage is unavailable", limits_);
    } catch (const std::filesystem::filesystem_error&) {
        return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                         "artifact storage is unavailable", limits_);
    } catch (const std::exception&) {
        return api_error(HttpStatus::ServiceUnavailable, "storage_failure",
                         "artifact storage is unavailable", limits_);
    }
}

Result<HttpResponse> ArtifactHttpApi::download(
    const http::HttpRequest&, const std::string& artifact_id) {
    if (!valid_artifact_id(artifact_id)) {
        return api_error(HttpStatus::BadRequest, "invalid_artifact_id",
                         "artifact id is invalid", limits_);
    }
    auto found = catalog_->find(artifact_id);
    if (!found) return api_error(HttpStatus::ServiceUnavailable, "catalog_failure",
                                "artifact catalog is unavailable", limits_);
    if (!found.value().has_value()) {
        return api_error(HttpStatus::NotFound, "artifact_not_found",
                         "artifact was not found", limits_);
    }
    const auto& entry = *found.value();
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(entry.canonical_path, error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return api_error(HttpStatus::NotFound, "artifact_not_found",
                             "artifact was not found", limits_);
        }
        const auto canonical = std::filesystem::canonical(entry.canonical_path, error);
        if (error || canonical != entry.canonical_path) {
            return api_error(HttpStatus::NotFound, "artifact_not_found",
                             "artifact was not found", limits_);
        }
        const auto size = std::filesystem::file_size(canonical, error);
        if (error || size != entry.artifact.size_bytes ||
            size > limits_.max_response_body_bytes()) {
            return api_error(size > limits_.max_response_body_bytes()
                                 ? HttpStatus::PayloadTooLarge
                                 : HttpStatus::ServiceUnavailable,
                             "artifact_unavailable",
                             "artifact cannot be served", limits_);
        }
        std::ifstream input(canonical, std::ios::binary);
        std::string body(static_cast<std::size_t>(size), '\0');
        input.read(body.data(), static_cast<std::streamsize>(body.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(body.size())) {
            return api_error(HttpStatus::ServiceUnavailable, "artifact_unavailable",
                             "artifact cannot be served", limits_);
        }
        const auto after_status = std::filesystem::symlink_status(
            entry.canonical_path, error);
        const auto after_canonical = std::filesystem::canonical(
            entry.canonical_path, error);
        const auto after_size = std::filesystem::file_size(
            entry.canonical_path, error);
        if (error || std::filesystem::is_symlink(after_status) ||
            !std::filesystem::is_regular_file(after_status) ||
            after_canonical != canonical || after_size != entry.artifact.size_bytes) {
            return api_error(HttpStatus::ServiceUnavailable, "artifact_unavailable",
                             "artifact cannot be served", limits_);
        }
        auto digest = sha256_text(body);
        if (!digest || digest.value() != entry.artifact.sha256) {
            return api_error(HttpStatus::ServiceUnavailable, "artifact_unavailable",
                             "artifact cannot be served", limits_);
        }
        HttpResponse response{HttpStatus::Ok};
        auto type = response.set_header("Content-Type", entry.artifact.media_type);
        if (!type) return Result<HttpResponse>::failure(type.error());
        auto disposition = response.set_header(
            "Content-Disposition", "attachment; filename=\"" +
            entry.safe_filename + "\"");
        if (!disposition) return Result<HttpResponse>::failure(disposition.error());
        auto nosniff = response.set_header("X-Content-Type-Options", "nosniff");
        if (!nosniff) return Result<HttpResponse>::failure(nosniff.error());
        auto cache = response.set_header("Cache-Control", "no-store");
        if (!cache) return Result<HttpResponse>::failure(cache.error());
        response.set_body(std::move(body));
        return Result<HttpResponse>::success(std::move(response));
    } catch (const std::bad_alloc&) {
        return api_error(HttpStatus::ServiceUnavailable, "artifact_unavailable",
                         "artifact cannot be served", limits_);
    } catch (const std::filesystem::filesystem_error&) {
        return api_error(HttpStatus::ServiceUnavailable, "artifact_unavailable",
                         "artifact cannot be served", limits_);
    } catch (const std::exception&) {
        return api_error(HttpStatus::ServiceUnavailable, "artifact_unavailable",
                         "artifact cannot be served", limits_);
    }
}

}  // namespace iaisf::application
