#pragma once

#include <string_view>

namespace iaisf::http {

enum class HttpStatus : int {
    Ok = 200,
    BadRequest = 400,
    NotFound = 404,
    MethodNotAllowed = 405,
    PayloadTooLarge = 413,
    UriTooLong = 414,
    ExpectationFailed = 417,
    RequestHeaderFieldsTooLarge = 431,
    InternalServerError = 500,
    NotImplemented = 501,
    ServiceUnavailable = 503,
    HttpVersionNotSupported = 505,
};

[[nodiscard]] std::string_view reason_phrase(HttpStatus status) noexcept;

}  // namespace iaisf::http
