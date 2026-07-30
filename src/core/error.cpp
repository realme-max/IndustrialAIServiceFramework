#include "iaisf/core/error.hpp"

namespace iaisf {

std::string_view to_string(const ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidArgument:
            return "invalid_argument";
        case ErrorCode::ConfigError:
            return "config_error";
        case ErrorCode::IoError:
            return "io_error";
        case ErrorCode::SystemError:
            return "system_error";
        case ErrorCode::InvalidState:
            return "invalid_state";
        case ErrorCode::ResourceExhausted:
            return "resource_exhausted";
        case ErrorCode::InternalError:
            return "internal_error";
    }
    return "unknown_error";
}

}  // namespace iaisf

