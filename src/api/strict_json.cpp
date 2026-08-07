#include "iaisf/api/strict_json.hpp"

#include <cmath>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iaisf::api {
namespace {

using Json = nlohmann::json;

StrictJsonError make_error(
    const StrictJsonFailureCategory category,
    const char* message) {
    return StrictJsonError{category, message};
}

class PreflightSax final : public Json::json_sax_t {
public:
    explicit PreflightSax(const StrictJsonLimits& limits) noexcept
        : limits_(limits) {}

    [[nodiscard]] const std::optional<StrictJsonError>& error() const noexcept {
        return error_;
    }

    bool null() override { return node(); }
    bool boolean(bool) override { return node(); }
    bool number_integer(number_integer_t) override { return node(); }
    bool number_unsigned(number_unsigned_t) override { return node(); }

    bool number_float(number_float_t value, const string_t&) override {
        if (!std::isfinite(value)) {
            return fail(StrictJsonFailureCategory::MalformedJson,
                        "non-finite numbers are not allowed");
        }
        return node();
    }

    bool string(string_t& value) override {
        if (!add_string_bytes(value.size())) {
            return false;
        }
        return node();
    }

    bool binary(binary_t&) override {
        return fail(StrictJsonFailureCategory::MalformedJson,
                    "binary JSON values are not allowed");
    }

    bool start_object(std::size_t) override {
        if (!node()) {
            return false;
        }
        if (stack_.size() + 1U > limits_.max_depth) {
            return fail(StrictJsonFailureCategory::DepthExceeded,
                        "JSON nesting depth exceeds the configured limit");
        }
        try {
            stack_.push_back(Frame{});
            return true;
        } catch (const std::bad_alloc&) {
            return fail(StrictJsonFailureCategory::ResourceFailure,
                        "unable to allocate JSON preflight state");
        }
    }

    bool key(string_t& value) override {
        if (!add_key_bytes(value.size())) {
            return false;
        }
        if (stack_.empty() || !stack_.back().object) {
            return fail(StrictJsonFailureCategory::MalformedJson,
                        "JSON key is outside an object");
        }
        try {
            if (!stack_.back().keys.insert(value).second) {
                return fail(StrictJsonFailureCategory::DuplicateKey,
                            "duplicate JSON object key");
            }
            return true;
        } catch (const std::bad_alloc&) {
            return fail(StrictJsonFailureCategory::ResourceFailure,
                        "unable to allocate JSON key state");
        }
    }

    bool end_object() override {
        if (stack_.empty() || !stack_.back().object) {
            return fail(StrictJsonFailureCategory::MalformedJson,
                        "unbalanced JSON object");
        }
        stack_.pop_back();
        return true;
    }

    bool start_array(std::size_t) override {
        if (!node()) {
            return false;
        }
        if (stack_.size() + 1U > limits_.max_depth) {
            return fail(StrictJsonFailureCategory::DepthExceeded,
                        "JSON nesting depth exceeds the configured limit");
        }
        try {
            stack_.push_back(Frame{false, {}});
            return true;
        } catch (const std::bad_alloc&) {
            return fail(StrictJsonFailureCategory::ResourceFailure,
                        "unable to allocate JSON preflight state");
        }
    }

    bool end_array() override {
        if (stack_.empty() || stack_.back().object) {
            return fail(StrictJsonFailureCategory::MalformedJson,
                        "unbalanced JSON array");
        }
        stack_.pop_back();
        return true;
    }

    bool parse_error(
        std::size_t,
        const std::string&,
        const nlohmann::detail::exception&) override {
        return fail(StrictJsonFailureCategory::MalformedJson,
                    "input is not valid strict JSON");
    }

private:
    struct Frame {
        bool object{true};
        std::unordered_set<std::string> keys;
    };

    bool fail(const StrictJsonFailureCategory category, const char* message) {
        if (!error_.has_value()) {
            error_ = StrictJsonError{category, message};
        }
        return false;
    }

    bool node() {
        if (nodes_ >= limits_.max_nodes) {
            return fail(StrictJsonFailureCategory::NodeLimitExceeded,
                        "JSON node count exceeds the configured limit");
        }
        ++nodes_;
        if (stack_.size() + 1U > limits_.max_depth) {
            return fail(StrictJsonFailureCategory::DepthExceeded,
                        "JSON nesting depth exceeds the configured limit");
        }
        return true;
    }

    bool add_key_bytes(const std::size_t amount) {
        if (amount > limits_.max_key_bytes - key_bytes_) {
            return fail(StrictJsonFailureCategory::TextLimitExceeded,
                        "JSON key bytes exceed the configured limit");
        }
        key_bytes_ += amount;
        return true;
    }

    bool add_string_bytes(const std::size_t amount) {
        if (amount > limits_.max_string_bytes - string_bytes_) {
            return fail(StrictJsonFailureCategory::TextLimitExceeded,
                        "JSON string bytes exceed the configured limit");
        }
        string_bytes_ += amount;
        return true;
    }

    const StrictJsonLimits& limits_;
    std::vector<Frame> stack_;
    std::optional<StrictJsonError> error_;
    std::size_t nodes_{0U};
    std::size_t key_bytes_{0U};
    std::size_t string_bytes_{0U};
};

}  // namespace

std::string_view to_string(
    const StrictJsonFailureCategory category) noexcept {
    switch (category) {
        case StrictJsonFailureCategory::MalformedJson:
            return "malformed_json";
        case StrictJsonFailureCategory::DuplicateKey:
            return "duplicate_key";
        case StrictJsonFailureCategory::DepthExceeded:
            return "depth_exceeded";
        case StrictJsonFailureCategory::NodeLimitExceeded:
            return "node_limit_exceeded";
        case StrictJsonFailureCategory::TextLimitExceeded:
            return "text_limit_exceeded";
        case StrictJsonFailureCategory::PayloadTooLarge:
            return "payload_too_large";
        case StrictJsonFailureCategory::ResourceFailure:
            return "resource_failure";
        case StrictJsonFailureCategory::InternalFailure:
            return "internal_failure";
    }
    return "internal_failure";
}

StrictJsonResult::StrictJsonResult(nlohmann::json value)
    : value_(std::move(value)) {}

StrictJsonResult::StrictJsonResult(StrictJsonError error)
    : error_(std::move(error)) {}

StrictJsonResult StrictJsonResult::success(nlohmann::json value) {
    return StrictJsonResult{std::move(value)};
}

StrictJsonResult StrictJsonResult::failure(StrictJsonError error) {
    return StrictJsonResult{std::move(error)};
}

bool StrictJsonResult::has_value() const noexcept {
    return value_.has_value();
}

StrictJsonResult::operator bool() const noexcept {
    return has_value();
}

nlohmann::json& StrictJsonResult::value() {
    return value_.value();
}

const nlohmann::json& StrictJsonResult::value() const {
    return value_.value();
}

StrictJsonError& StrictJsonResult::error() {
    return error_.value();
}

const StrictJsonError& StrictJsonResult::error() const {
    return error_.value();
}

StrictJsonResult parse_strict_json(
    const std::string_view input,
    const StrictJsonLimits limits) {
    try {
        if (limits.max_bytes == 0U || limits.max_depth == 0U ||
            limits.max_nodes == 0U || limits.max_key_bytes == 0U ||
            limits.max_string_bytes == 0U) {
            return StrictJsonResult::failure(make_error(
                StrictJsonFailureCategory::InternalFailure,
                "strict JSON limits are invalid"));
        }
        if (input.size() > limits.max_bytes) {
            return StrictJsonResult::failure(make_error(
                StrictJsonFailureCategory::PayloadTooLarge,
                "JSON request exceeds the configured byte limit"));
        }

        PreflightSax preflight{limits};
        if (!Json::sax_parse(
                input.begin(),
                input.end(),
                &preflight,
                Json::input_format_t::json,
                true)) {
            if (preflight.error().has_value()) {
                return StrictJsonResult::failure(*preflight.error());
            }
            return StrictJsonResult::failure(make_error(
                StrictJsonFailureCategory::MalformedJson,
                "input is not valid strict JSON"));
        }
        auto value = Json::parse(input.begin(), input.end(), nullptr, false);
        if (value.is_discarded()) {
            return StrictJsonResult::failure(make_error(
                StrictJsonFailureCategory::MalformedJson,
                "input is not valid strict JSON"));
        }
        return StrictJsonResult::success(std::move(value));
    } catch (const std::bad_alloc&) {
        return StrictJsonResult::failure(make_error(
            StrictJsonFailureCategory::ResourceFailure,
            "unable to allocate strict JSON storage"));
    } catch (const std::length_error&) {
        return StrictJsonResult::failure(make_error(
            StrictJsonFailureCategory::ResourceFailure,
            "strict JSON exceeds the platform size limit"));
    } catch (const std::exception&) {
        return StrictJsonResult::failure(make_error(
            StrictJsonFailureCategory::InternalFailure,
            "strict JSON processing failed"));
    }
}

}  // namespace iaisf::api
