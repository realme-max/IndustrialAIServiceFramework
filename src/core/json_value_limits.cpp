#include "iaisf/core/json_value_limits.hpp"

#include <cmath>
#include <new>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>

#include <nlohmann/json.hpp>

namespace iaisf {
namespace {

class JsonSizeLimitExceeded final {};

class CountingStreamBuffer final : public std::streambuf {
public:
    explicit CountingStreamBuffer(const std::size_t maximum) noexcept
        : maximum_(maximum) {}

    [[nodiscard]] std::size_t count() const noexcept {
        return count_;
    }

    [[nodiscard]] bool exceeded() const noexcept {
        return exceeded_;
    }

protected:
    std::streamsize xsputn(
        const char*,
        const std::streamsize count) override {
        if (count > 0) {
            add(static_cast<std::size_t>(count));
        }
        return count;
    }

    int_type overflow(const int_type character) override {
        if (!traits_type::eq_int_type(character, traits_type::eof())) {
            add(1U);
        }
        return traits_type::not_eof(character);
    }

private:
    void add(const std::size_t amount) {
        if (exceeded_) {
            throw JsonSizeLimitExceeded{};
        }
        if (amount > maximum_ - count_) {
            exceeded_ = true;
            throw JsonSizeLimitExceeded{};
        }
        count_ += amount;
    }

    std::size_t maximum_;
    std::size_t count_{0};
    bool exceeded_{false};
};

Result<void> capacity_error(
    const std::string_view value_name,
    const std::string_view detail) {
    return Result<void>::failure(make_error(
        ErrorCode::ResourceExhausted,
        std::string(value_name) + " " + std::string(detail)));
}

Result<void> invalid_value_error(
    const std::string_view value_name,
    const std::string_view detail,
    const ErrorCode code) {
    return Result<void>::failure(make_error(
        code,
        std::string(value_name) + " " + std::string(detail)));
}

Result<void> inspect_node(
    const nlohmann::json& value,
    const JsonValueLimits& limits,
    const std::string_view value_name,
    const ErrorCode invalid_value_code,
    const std::size_t depth,
    std::size_t& elements) {
    if (depth > limits.max_depth) {
        return capacity_error(value_name, "exceeds the configured depth limit");
    }
    if (elements >= limits.max_elements) {
        return capacity_error(
            value_name,
            "exceeds the configured element limit");
    }
    ++elements;

    if (value.is_discarded()) {
        return invalid_value_error(
            value_name,
            "contains a discarded JSON value",
            invalid_value_code);
    }
    if (value.is_binary()) {
        return invalid_value_error(
            value_name,
            "contains a non-JSON binary value",
            invalid_value_code);
    }
    if (value.is_number_float() &&
        !std::isfinite(value.get<nlohmann::json::number_float_t>())) {
        return invalid_value_error(
            value_name,
            "contains a non-finite number",
            invalid_value_code);
    }
    if (value.is_string() &&
        value.get_ref<const nlohmann::json::string_t&>().size() >
            limits.max_string_bytes) {
        return capacity_error(
            value_name,
            "contains a string exceeding the configured byte limit");
    }
    if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            if (item.key().size() > limits.max_string_bytes) {
                return capacity_error(
                    value_name,
                    "contains an object key exceeding the configured byte limit");
            }
            auto child = inspect_node(
                item.value(),
                limits,
                value_name,
                invalid_value_code,
                depth + 1U,
                elements);
            if (!child) {
                return child;
            }
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            auto child = inspect_node(
                item,
                limits,
                value_name,
                invalid_value_code,
                depth + 1U,
                elements);
            if (!child) {
                return child;
            }
        }
    }
    return Result<void>::success();
}

}  // namespace

Result<std::size_t> validate_json_value(
    const nlohmann::json& value,
    const JsonValueLimits& limits,
    const std::string_view value_name,
    const ErrorCode invalid_value_code) {
    try {
        std::size_t elements = 0;
        auto structural = inspect_node(
            value,
            limits,
            value_name,
            invalid_value_code,
            1U,
            elements);
        if (!structural) {
            return Result<std::size_t>::failure(
                std::move(structural).error());
        }

        CountingStreamBuffer buffer{limits.max_serialized_bytes};
        std::ostream output{&buffer};
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << value;
        return Result<std::size_t>::success(buffer.count());
    } catch (const JsonSizeLimitExceeded&) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            std::string(value_name) +
                " exceeds the configured serialized byte limit"));
    } catch (const nlohmann::json::exception&) {
        return Result<std::size_t>::failure(make_error(
            invalid_value_code,
            std::string(value_name) +
                " cannot be serialized as valid UTF-8"));
    } catch (const std::bad_alloc&) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            std::string("unable to allocate ") + std::string(value_name) +
                " validation storage"));
    } catch (const std::length_error&) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            std::string(value_name) +
                " exceeds the platform size limit"));
    } catch (const std::exception&) {
        return Result<std::size_t>::failure(make_error(
            invalid_value_code,
            std::string(value_name) + " is not a valid JSON value"));
    }
}

}  // namespace iaisf
