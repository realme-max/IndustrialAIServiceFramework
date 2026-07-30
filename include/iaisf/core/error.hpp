#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace iaisf {

enum class ErrorCode {
    InvalidArgument,
    ConfigError,
    IoError,
    SystemError,
    InvalidState,
    ResourceExhausted,
    InternalError,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/**
 * Mutable error value used at Phase 1 API boundaries.
 *
 * The constructors and make_error() normalize an empty message. Because the
 * fields remain public value fields, callers can mutate message afterwards;
 * Result::failure() therefore normalizes it again at the result boundary.
 */
struct Error {
    ErrorCode code{ErrorCode::InternalError};
    std::string message{"unspecified error"};

    Error() = default;

    Error(ErrorCode error_code, std::string error_message)
        : code(error_code),
          message(error_message.empty() ? "unspecified error" : std::move(error_message)) {}
};

[[nodiscard]] inline Error make_error(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

}  // namespace iaisf
