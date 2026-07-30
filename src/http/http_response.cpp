#include "iaisf/http/http_response.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace iaisf::http {
namespace {

[[nodiscard]] bool is_token_character(unsigned char value) noexcept {
    if ((value >= static_cast<unsigned char>('a') &&
         value <= static_cast<unsigned char>('z')) ||
        (value >= static_cast<unsigned char>('A') &&
         value <= static_cast<unsigned char>('Z')) ||
        (value >= static_cast<unsigned char>('0') &&
         value <= static_cast<unsigned char>('9'))) {
        return true;
    }
    constexpr std::string_view kPunctuation{"!#$%&'*+-.^_`|~"};
    return kPunctuation.find(static_cast<char>(value)) != std::string_view::npos;
}

[[nodiscard]] std::string lowercase(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(
            byte >= static_cast<unsigned char>('A') &&
                    byte <= static_cast<unsigned char>('Z')
                ? static_cast<char>(
                      byte + static_cast<unsigned char>('a' - 'A'))
                : character);
    }
    return result;
}

[[nodiscard]] bool valid_header_value(std::string_view value) noexcept {
    return std::none_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte < 0x20U && byte != 0x09U) || byte == 0x7FU;
    });
}

[[nodiscard]] bool can_add(std::size_t left, std::size_t right) noexcept {
    return right <= std::numeric_limits<std::size_t>::max() - left;
}

[[nodiscard]] Result<std::size_t> checked_sum(
    const std::size_t left,
    const std::size_t right,
    const char* const message) {
    if (!can_add(left, right)) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            message));
    }
    return Result<std::size_t>::success(left + right);
}

[[nodiscard]] Result<std::size_t> header_line_size(
    const std::string_view name,
    const std::string_view value) {
    auto size = checked_sum(
        name.size(),
        2U,
        "HTTP response header line size overflow");
    if (!size) {
        return size;
    }
    size = checked_sum(
        size.value(),
        value.size(),
        "HTTP response header line size overflow");
    if (!size) {
        return size;
    }
    return checked_sum(
        size.value(),
        2U,
        "HTTP response header line size overflow");
}

[[nodiscard]] Result<std::size_t> add_header_line(
    const std::size_t current_head_size,
    const std::string_view name,
    const std::string_view value,
    const HttpLimits& limits) {
    auto line_size = header_line_size(name, value);
    if (!line_size) {
        return line_size;
    }
    if (line_size.value() > limits.max_header_line_bytes()) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response header line exceeds configured limit"));
    }
    auto total = checked_sum(
        current_head_size,
        line_size.value(),
        "HTTP response header total size overflow");
    if (!total) {
        return total;
    }
    if (total.value() > limits.max_header_bytes()) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response headers exceed configured limit"));
    }
    return total;
}

[[nodiscard]] Result<std::size_t> validated_head_size(
    const HttpStatus status,
    const std::vector<HttpHeader>& headers,
    const std::size_t body_size,
    const bool close_connection,
    const HttpLimits& limits) {
    constexpr std::size_t kAutomaticHeaderCount = 2U;
    if (headers.size() >
        std::numeric_limits<std::size_t>::max() - kAutomaticHeaderCount) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response header count overflow"));
    }
    if (headers.size() + kAutomaticHeaderCount >
        limits.max_header_count()) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response header count exceeds configured limit"));
    }
    if (body_size > limits.max_response_body_bytes()) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response body exceeds configured limit"));
    }

    const std::string status_code =
        std::to_string(static_cast<int>(status));
    std::size_t head_size = 0U;
    const std::string_view status_prefix{"HTTP/1.1 "};
    const std::string_view reason = reason_phrase(status);
    const std::array<std::size_t, 5U> status_component_sizes{
        status_prefix.size(),
        status_code.size(),
        1U,
        reason.size(),
        2U};
    for (const auto component_size : status_component_sizes) {
        auto total = checked_sum(
            head_size,
            component_size,
            "HTTP response status line size overflow");
        if (!total) {
            return total;
        }
        head_size = total.value();
    }
    if (head_size > limits.max_header_bytes()) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response status line exceeds configured header total"));
    }

    for (const auto& header : headers) {
        if (header.name.empty() ||
            !std::all_of(
                header.name.begin(),
                header.name.end(),
                [](const char character) {
                    return is_token_character(
                        static_cast<unsigned char>(character));
                }) ||
            !valid_header_value(header.value)) {
            return Result<std::size_t>::failure(make_error(
                ErrorCode::InvalidArgument,
                "HTTP response contains an invalid header"));
        }
        auto total = add_header_line(
            head_size,
            header.name,
            header.value,
            limits);
        if (!total) {
            return total;
        }
        head_size = total.value();
    }

    const std::string content_length = std::to_string(body_size);
    auto total = add_header_line(
        head_size,
        "Content-Length",
        content_length,
        limits);
    if (!total) {
        return total;
    }
    head_size = total.value();
    total = add_header_line(
        head_size,
        "Connection",
        close_connection ? std::string_view{"close"}
                         : std::string_view{"keep-alive"},
        limits);
    if (!total) {
        return total;
    }
    head_size = total.value();

    total = checked_sum(
        head_size,
        2U,
        "HTTP response header terminator size overflow");
    if (!total) {
        return total;
    }
    head_size = total.value();
    if (head_size > limits.max_header_bytes()) {
        return Result<std::size_t>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response headers exceed configured limit"));
    }
    auto serialized_size = checked_sum(
        head_size,
        body_size,
        "HTTP serialized response size overflow");
    if (!serialized_size) {
        return serialized_size;
    }
    return Result<std::size_t>::success(head_size);
}

}  // namespace

HttpResponse::HttpResponse(HttpStatus status) noexcept : status_(status) {}

HttpStatus HttpResponse::status() const noexcept {
    return status_;
}
const std::vector<HttpHeader>& HttpResponse::headers() const noexcept {
    return headers_;
}
const std::string& HttpResponse::body() const noexcept {
    return body_;
}
bool HttpResponse::close_connection() const noexcept {
    return close_connection_;
}

Result<void> HttpResponse::set_header(std::string name, std::string value) {
    if (name.empty() ||
        !std::all_of(name.begin(), name.end(), [](char character) {
            return is_token_character(static_cast<unsigned char>(character));
        })) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP response header name is invalid"));
    }
    if (!valid_header_value(value)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP response header value contains a control character"));
    }
    auto hard_line_size = header_line_size(name, value);
    if (!hard_line_size ||
        hard_line_size.value() >
            static_cast<std::size_t>(HttpLimits::kMaximumByteLimit)) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response header exceeds the hard byte limit"));
    }

    try {
        const auto normalized = lowercase(name);
        if (normalized == "content-length" || normalized == "connection" ||
            normalized == "transfer-encoding") {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "HTTP framing headers are owned by the framework"));
        }
        const auto found = std::find_if(
            headers_.begin(),
            headers_.end(),
            [&normalized](const HttpHeader& header) {
                return lowercase(header.name) == normalized;
            });
        if (found != headers_.end()) {
            found->name = std::move(name);
            found->value = std::move(value);
        } else {
            if (headers_.size() >=
                static_cast<std::size_t>(
                    HttpLimits::kMaximumCountLimit - 2)) {
                return Result<void>::failure(make_error(
                    ErrorCode::ResourceExhausted,
                    "HTTP response header count exceeds the hard limit"));
            }
            headers_.push_back(HttpHeader{std::move(name), std::move(value)});
        }
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response header allocation failed"));
    }
    return Result<void>::success();
}

void HttpResponse::set_body(std::string body) noexcept {
    body_ = std::move(body);
}

void HttpResponse::set_close_connection(bool close) noexcept {
    close_connection_ = close;
}

Result<void> HttpResponse::validate(const HttpLimits& limits) const {
    try {
        auto head_size = validated_head_size(
            status_,
            headers_,
            body_.size(),
            close_connection_,
            limits);
        if (!head_size) {
            return Result<void>::failure(std::move(head_size).error());
        }
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response validation allocation failed"));
    } catch (const std::length_error&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response validation length failed"));
    }
}

Result<std::string> HttpResponse::serialize(const HttpLimits& limits) const {
    try {
        auto validated = validated_head_size(
            status_,
            headers_,
            body_.size(),
            close_connection_,
            limits);
        if (!validated) {
            return Result<std::string>::failure(
                std::move(validated).error());
        }
        std::string head;
        head.reserve(validated.value());
        head.append("HTTP/1.1 ");
        head.append(std::to_string(static_cast<int>(status_)));
        head.push_back(' ');
        head.append(reason_phrase(status_));
        head.append("\r\n");
        for (const auto& header : headers_) {
            if (!can_add(head.size(), header.name.size()) ||
                !can_add(head.size() + header.name.size(), header.value.size() + 4U)) {
                return Result<std::string>::failure(make_error(
                    ErrorCode::ResourceExhausted,
                    "HTTP response header size overflow"));
            }
            head.append(header.name);
            head.append(": ");
            head.append(header.value);
            head.append("\r\n");
        }
        head.append("Content-Length: ");
        head.append(std::to_string(body_.size()));
        head.append("\r\nConnection: ");
        head.append(close_connection_ ? "close" : "keep-alive");
        head.append("\r\n\r\n");

        if (head.size() != validated.value()) {
            std::terminate();
        }
        head.reserve(head.size() + body_.size());
        head.append(body_.data(), body_.size());
        return Result<std::string>::success(std::move(head));
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response serialization allocation failed"));
    } catch (const std::length_error&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP response serialization length failed"));
    }
}

HttpResponse HttpResponse::error(
    HttpStatus status,
    bool close_connection) {
    HttpResponse response{status};
    const auto content_type =
        response.set_header(
            "Content-Type",
            "text/plain; charset=utf-8");
    if (!content_type) {
        response.set_close_connection(true);
        return response;
    }
    std::string body;
    body.append(reason_phrase(status));
    body.push_back('\n');
    response.set_body(std::move(body));
    response.set_close_connection(close_connection);
    return response;
}

}  // namespace iaisf::http
