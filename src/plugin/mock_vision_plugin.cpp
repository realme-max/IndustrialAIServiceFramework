#include "iaisf/plugin/mock_vision_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <new>
#include <string_view>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::plugin {
namespace {

constexpr std::size_t kMaxImageIdBytes = 128;
constexpr std::int64_t kMaxDimension = 16384;
constexpr double kDefaultThreshold = 0.5;
constexpr double kMockConfidence = 0.93;

bool contains_control(const std::string_view value) noexcept {
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte == 0x7FU) {
            return true;
        }
    }
    return false;
}

bool is_known_field(const std::string_view field) noexcept {
    return field == "image_id" || field == "width" || field == "height" ||
           field == "confidence_threshold";
}

Result<std::int64_t> read_dimension(
    const nlohmann::json& input,
    const std::string_view field) {
    const auto found = input.find(field);
    if (found == input.end()) {
        return Result<std::int64_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " is required"));
    }
    if (!found->is_number_integer() && !found->is_number_unsigned()) {
        return Result<std::int64_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " must be an integer"));
    }

    try {
        if (found->is_number_unsigned()) {
            const auto value = found->get<std::uint64_t>();
            if (value == 0U ||
                value > static_cast<std::uint64_t>(kMaxDimension)) {
                return Result<std::int64_t>::failure(make_error(
                    ErrorCode::InvalidArgument,
                    std::string(field) + " is outside the supported range"));
            }
            return Result<std::int64_t>::success(
                static_cast<std::int64_t>(value));
        }
        const auto value = found->get<std::int64_t>();
        if (value <= 0 || value > kMaxDimension) {
            return Result<std::int64_t>::failure(make_error(
                ErrorCode::InvalidArgument,
                std::string(field) + " is outside the supported range"));
        }
        return Result<std::int64_t>::success(value);
    } catch (const std::exception&) {
        return Result<std::int64_t>::failure(make_error(
            ErrorCode::InvalidArgument,
            std::string(field) + " is outside the supported range"));
    }
}

Result<double> read_threshold(const nlohmann::json& input) {
    const auto found = input.find("confidence_threshold");
    if (found == input.end()) {
        return Result<double>::success(kDefaultThreshold);
    }
    if (!found->is_number()) {
        return Result<double>::failure(make_error(
            ErrorCode::InvalidArgument,
            "confidence_threshold must be numeric"));
    }
    try {
        const double value = found->get<double>();
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return Result<double>::failure(make_error(
                ErrorCode::InvalidArgument,
                "confidence_threshold is outside the supported range"));
        }
        return Result<double>::success(value);
    } catch (const std::exception&) {
        return Result<double>::failure(make_error(
            ErrorCode::InvalidArgument,
            "confidence_threshold is outside the supported range"));
    }
}

}  // namespace

PluginMetadata MockVisionPlugin::metadata() const {
    return PluginMetadata{
        "mock_vision.detect",
        "Mock Vision Plugin",
        "1.0.0",
        "Deterministic mock only; it reads no file and runs no real inference.",
        true,
        {"deterministic", "mock", "stateless"},
    };
}

Result<void> MockVisionPlugin::validate_input(
    const nlohmann::json& input) const {
    if (!input.is_object()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "mock vision input must be an object"));
    }
    for (auto item = input.begin(); item != input.end(); ++item) {
        if (!is_known_field(item.key())) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "mock vision input contains an unknown field"));
        }
    }

    const auto image_id = input.find("image_id");
    if (image_id == input.end() || !image_id->is_string()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "image_id must be a string"));
    }
    const auto& image_id_value =
        image_id->get_ref<const nlohmann::json::string_t&>();
    if (image_id_value.empty()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "image_id must not be empty"));
    }
    if (image_id_value.size() > kMaxImageIdBytes) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "image_id exceeds its byte limit"));
    }
    if (contains_control(image_id_value)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "image_id contains a control character"));
    }

    auto width = read_dimension(input, "width");
    if (!width) {
        return Result<void>::failure(std::move(width).error());
    }
    auto height = read_dimension(input, "height");
    if (!height) {
        return Result<void>::failure(std::move(height).error());
    }
    auto threshold = read_threshold(input);
    if (!threshold) {
        return Result<void>::failure(std::move(threshold).error());
    }
    return Result<void>::success();
}

Result<nlohmann::json> MockVisionPlugin::execute(
    const nlohmann::json& input) const {
    auto valid = validate_input(input);
    if (!valid) {
        return Result<nlohmann::json>::failure(std::move(valid).error());
    }

    try {
        const auto& image_id =
            input.at("image_id").get_ref<const nlohmann::json::string_t&>();
        const auto width = read_dimension(input, "width").value();
        const auto height = read_dimension(input, "height").value();
        const auto threshold = read_threshold(input).value();

        nlohmann::json detections = nlohmann::json::array();
        if (threshold <= kMockConfidence) {
            detections.push_back({
                {"type", "weld_seam"},
                {"confidence", kMockConfidence},
                {
                    "bbox",
                    {
                        {"x", width / 4},
                        {"y", height / 3},
                        {"width", std::max<std::int64_t>(1, width / 2)},
                        {"height", std::max<std::int64_t>(1, height / 10)},
                    },
                },
            });
        }

        return Result<nlohmann::json>::success({
            {"mock", true},
            {"operation", "mock_vision.detect"},
            {"image_id", image_id},
            {
                "image_size",
                {
                    {"width", width},
                    {"height", height},
                },
            },
            {"detections", std::move(detections)},
        });
    } catch (const std::bad_alloc&) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate mock vision result"));
    } catch (const std::exception&) {
        return Result<nlohmann::json>::failure(make_error(
            ErrorCode::InternalError,
            "mock vision execution failed"));
    }
}

Result<void> MockVisionPlugin::initialize() {
    return Result<void>::success();
}

Result<void> MockVisionPlugin::shutdown() {
    return Result<void>::success();
}

}  // namespace iaisf::plugin
