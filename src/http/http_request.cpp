#include "iaisf/http/http_request.hpp"

#include <algorithm>
#include <cstddef>
#include <new>
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

[[nodiscard]] bool is_lowercase_header_name(const std::string& name) noexcept {
    return !name.empty() &&
           std::all_of(name.begin(), name.end(), [](char character) {
               const auto value = static_cast<unsigned char>(character);
               return is_token_character(value) &&
                      !(value >= static_cast<unsigned char>('A') &&
                        value <= static_cast<unsigned char>('Z'));
           });
}

[[nodiscard]] bool valid_method(const std::string& method) noexcept {
    return !method.empty() &&
           std::all_of(method.begin(), method.end(), [](char character) {
               return is_token_character(static_cast<unsigned char>(character));
           });
}

[[nodiscard]] bool valid_target(const std::string& target) noexcept {
    if (target.empty() || target.front() != '/' ||
        target.find('#') != std::string::npos ||
        target.find('\\') != std::string::npos) {
        return false;
    }
    return std::none_of(target.begin(), target.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value <= 0x20U || value == 0x7FU;
    });
}

[[nodiscard]] bool valid_header_value(const std::string& value) noexcept {
    return std::none_of(value.begin(), value.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte < 0x20U && byte != 0x09U) || byte == 0x7FU;
    });
}

}  // namespace

Result<HttpRequest> HttpRequest::create(
    std::string method,
    std::string target,
    Headers headers,
    std::string body,
    bool keep_alive) {
    if (!valid_method(method)) {
        return Result<HttpRequest>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP method is not a valid token"));
    }
    if (!valid_target(target)) {
        return Result<HttpRequest>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP request target must be a valid origin-form target"));
    }
    for (std::size_t index = 0U; index < headers.size(); ++index) {
        const auto& header = headers[index];
        if (!is_lowercase_header_name(header.name) ||
            !valid_header_value(header.value)) {
            return Result<HttpRequest>::failure(make_error(
                ErrorCode::InvalidArgument,
                "HTTP request headers must be normalized and valid"));
        }
        const auto duplicate = std::find_if(
            headers.begin(),
            headers.begin() + static_cast<std::ptrdiff_t>(index),
            [&header](const HttpHeader& previous) {
                return previous.name == header.name;
            });
        if (duplicate !=
            headers.begin() + static_cast<std::ptrdiff_t>(index)) {
            return Result<HttpRequest>::failure(make_error(
                ErrorCode::InvalidArgument,
                "HTTP request headers must be unique"));
        }
    }

    const auto query_separator = target.find('?');
    std::string path;
    std::string query;
    try {
        path = target.substr(0U, query_separator);
        if (query_separator != std::string::npos) {
            query = target.substr(query_separator + 1U);
        }
    } catch (const std::bad_alloc&) {
        return Result<HttpRequest>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP request allocation failed"));
    }

    return Result<HttpRequest>::success(HttpRequest{
        std::move(method),
        std::move(target),
        std::move(path),
        std::move(query),
        std::move(headers),
        std::move(body),
        keep_alive});
}

HttpRequest::HttpRequest(
    std::string method,
    std::string target,
    std::string path,
    std::string query,
    Headers headers,
    std::string body,
    bool keep_alive) noexcept
    : method_(std::move(method)),
      target_(std::move(target)),
      path_(std::move(path)),
      query_(std::move(query)),
      headers_(std::move(headers)),
      body_(std::move(body)),
      keep_alive_(keep_alive) {}

const std::string& HttpRequest::method() const noexcept {
    return method_;
}
const std::string& HttpRequest::target() const noexcept {
    return target_;
}
const std::string& HttpRequest::path() const noexcept {
    return path_;
}
const std::string& HttpRequest::query() const noexcept {
    return query_;
}
const HttpRequest::Headers& HttpRequest::headers() const noexcept {
    return headers_;
}

std::optional<std::string> HttpRequest::header(
    const std::string& lowercase_name) const {
    const auto found = std::find_if(
        headers_.begin(),
        headers_.end(),
        [&lowercase_name](const HttpHeader& header) {
            return header.name == lowercase_name;
        });
    if (found == headers_.end()) {
        return std::nullopt;
    }
    return found->value;
}

const std::string& HttpRequest::body() const noexcept {
    return body_;
}
bool HttpRequest::keep_alive() const noexcept {
    return keep_alive_;
}

}  // namespace iaisf::http
