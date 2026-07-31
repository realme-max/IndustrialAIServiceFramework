#include "iaisf/http/http_status.hpp"

namespace iaisf::http {

std::string_view reason_phrase(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::Ok:
            return "OK";
        case HttpStatus::Accepted:
            return "Accepted";
        case HttpStatus::BadRequest:
            return "Bad Request";
        case HttpStatus::NotFound:
            return "Not Found";
        case HttpStatus::MethodNotAllowed:
            return "Method Not Allowed";
        case HttpStatus::PayloadTooLarge:
            return "Payload Too Large";
        case HttpStatus::UriTooLong:
            return "URI Too Long";
        case HttpStatus::UnsupportedMediaType:
            return "Unsupported Media Type";
        case HttpStatus::ExpectationFailed:
            return "Expectation Failed";
        case HttpStatus::UnprocessableContent:
            return "Unprocessable Content";
        case HttpStatus::RequestHeaderFieldsTooLarge:
            return "Request Header Fields Too Large";
        case HttpStatus::InternalServerError:
            return "Internal Server Error";
        case HttpStatus::NotImplemented:
            return "Not Implemented";
        case HttpStatus::ServiceUnavailable:
            return "Service Unavailable";
        case HttpStatus::HttpVersionNotSupported:
            return "HTTP Version Not Supported";
    }
    return "Unknown";
}

}  // namespace iaisf::http
