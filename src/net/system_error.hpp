#pragma once

#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::net::detail {

[[nodiscard]] inline Error make_system_error(
    const std::string_view operation,
    const int error_number) {
    std::string message{operation};
    message += " failed (errno ";
    message += std::to_string(error_number);
    message += ": ";
    message += std::error_code{error_number, std::generic_category()}.message();
    message += ')';
    return make_error(ErrorCode::SystemError, std::move(message));
}

}  // namespace iaisf::net::detail
