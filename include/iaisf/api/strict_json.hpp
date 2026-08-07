#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace iaisf::api {

struct StrictJsonLimits final {
    std::size_t max_bytes{4U * 1024U};
    std::size_t max_depth{8U};
    std::size_t max_nodes{128U};
    std::size_t max_key_bytes{2U * 1024U};
    std::size_t max_string_bytes{6U * 1024U};

    [[nodiscard]] static StrictJsonLimits defaults() noexcept {
        return {};
    }
};

enum class StrictJsonFailureCategory {
    MalformedJson,
    DuplicateKey,
    DepthExceeded,
    NodeLimitExceeded,
    TextLimitExceeded,
    PayloadTooLarge,
    ResourceFailure,
    InternalFailure,
};

[[nodiscard]] std::string_view to_string(
    StrictJsonFailureCategory category) noexcept;

struct StrictJsonError final {
    StrictJsonFailureCategory category;
    std::string message;
};

class StrictJsonResult final {
public:
    static StrictJsonResult success(nlohmann::json value);
    static StrictJsonResult failure(StrictJsonError error);

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] nlohmann::json& value();
    [[nodiscard]] const nlohmann::json& value() const;
    [[nodiscard]] StrictJsonError& error();
    [[nodiscard]] const StrictJsonError& error() const;

private:
    explicit StrictJsonResult(nlohmann::json value);
    explicit StrictJsonResult(StrictJsonError error);

    std::optional<nlohmann::json> value_;
    std::optional<StrictJsonError> error_;
};

/**
 * Performs a bounded SAX preflight (including duplicate-key detection) and
 * only then builds a DOM. The input is never echoed in an error.
 */
[[nodiscard]] StrictJsonResult parse_strict_json(
    std::string_view input,
    StrictJsonLimits limits = StrictJsonLimits::defaults());

}  // namespace iaisf::api
