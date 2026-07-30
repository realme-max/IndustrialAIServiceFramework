#include "iaisf/http/http_limits.hpp"

#include <array>
#include <limits>

namespace iaisf::http {
namespace {

[[nodiscard]] bool valid_byte_limit(std::int64_t value) noexcept {
    return value > 0 && value <= HttpLimits::kMaximumByteLimit;
}

[[nodiscard]] bool valid_count_limit(std::int64_t value) noexcept {
    return value > 0 && value <= HttpLimits::kMaximumCountLimit;
}

}  // namespace

Result<HttpLimits> HttpLimits::create(
    std::int64_t max_request_line_bytes,
    std::int64_t max_method_bytes,
    std::int64_t max_target_bytes,
    std::int64_t max_header_line_bytes,
    std::int64_t max_header_bytes,
    std::int64_t max_header_count,
    std::int64_t max_body_bytes,
    std::int64_t max_response_body_bytes,
    std::int64_t max_routes,
    std::int64_t max_requests_per_dispatch) {
    const std::array<std::int64_t, 7U> byte_limits{
        max_request_line_bytes,
        max_method_bytes,
        max_target_bytes,
        max_header_line_bytes,
        max_header_bytes,
        max_body_bytes,
        max_response_body_bytes,
    };
    for (const auto value : byte_limits) {
        if (!valid_byte_limit(value)) {
            return Result<HttpLimits>::failure(make_error(
                ErrorCode::InvalidArgument,
                "HTTP byte limits must be positive and no greater than 64 MiB"));
        }
    }
    if (!valid_count_limit(max_header_count) ||
        !valid_count_limit(max_routes) ||
        !valid_count_limit(max_requests_per_dispatch)) {
        return Result<HttpLimits>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP count limits must be positive and within the hard bound"));
    }

    constexpr std::int64_t kRequestLineSeparatorsVersionAndCrlf = 12;
    if (max_method_bytes >
        std::numeric_limits<std::int64_t>::max() - max_target_bytes -
            kRequestLineSeparatorsVersionAndCrlf) {
        return Result<HttpLimits>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP request-line limit relationship overflows"));
    }
    const auto minimum_request_line =
        max_method_bytes + max_target_bytes +
        kRequestLineSeparatorsVersionAndCrlf;
    if (max_request_line_bytes < minimum_request_line) {
        return Result<HttpLimits>::failure(make_error(
            ErrorCode::InvalidArgument,
            "request-line limit is smaller than method, target and version limits"));
    }
    if (max_header_line_bytes > max_header_bytes) {
        return Result<HttpLimits>::failure(make_error(
            ErrorCode::InvalidArgument,
            "header-line limit cannot exceed total header limit"));
    }
    if (max_header_line_bytes < 2) {
        return Result<HttpLimits>::failure(make_error(
            ErrorCode::InvalidArgument,
            "header-line limit must include CRLF"));
    }

    return Result<HttpLimits>::success(HttpLimits{
        static_cast<std::size_t>(max_request_line_bytes),
        static_cast<std::size_t>(max_method_bytes),
        static_cast<std::size_t>(max_target_bytes),
        static_cast<std::size_t>(max_header_line_bytes),
        static_cast<std::size_t>(max_header_bytes),
        static_cast<std::size_t>(max_header_count),
        static_cast<std::size_t>(max_body_bytes),
        static_cast<std::size_t>(max_response_body_bytes),
        static_cast<std::size_t>(max_routes),
        static_cast<std::size_t>(max_requests_per_dispatch)});
}

HttpLimits HttpLimits::defaults() noexcept {
    return HttpLimits{
        16U * 1024U,
        32U,
        8U * 1024U,
        8U * 1024U,
        32U * 1024U,
        100U,
        1024U * 1024U,
        1024U * 1024U,
        256U,
        16U};
}

HttpLimits::HttpLimits(
    std::size_t max_request_line_bytes,
    std::size_t max_method_bytes,
    std::size_t max_target_bytes,
    std::size_t max_header_line_bytes,
    std::size_t max_header_bytes,
    std::size_t max_header_count,
    std::size_t max_body_bytes,
    std::size_t max_response_body_bytes,
    std::size_t max_routes,
    std::size_t max_requests_per_dispatch) noexcept
    : max_request_line_bytes_(max_request_line_bytes),
      max_method_bytes_(max_method_bytes),
      max_target_bytes_(max_target_bytes),
      max_header_line_bytes_(max_header_line_bytes),
      max_header_bytes_(max_header_bytes),
      max_header_count_(max_header_count),
      max_body_bytes_(max_body_bytes),
      max_response_body_bytes_(max_response_body_bytes),
      max_routes_(max_routes),
      max_requests_per_dispatch_(max_requests_per_dispatch) {}

std::size_t HttpLimits::max_request_line_bytes() const noexcept {
    return max_request_line_bytes_;
}
std::size_t HttpLimits::max_method_bytes() const noexcept {
    return max_method_bytes_;
}
std::size_t HttpLimits::max_target_bytes() const noexcept {
    return max_target_bytes_;
}
std::size_t HttpLimits::max_header_line_bytes() const noexcept {
    return max_header_line_bytes_;
}
std::size_t HttpLimits::max_header_bytes() const noexcept {
    return max_header_bytes_;
}
std::size_t HttpLimits::max_header_count() const noexcept {
    return max_header_count_;
}
std::size_t HttpLimits::max_body_bytes() const noexcept {
    return max_body_bytes_;
}
std::size_t HttpLimits::max_response_body_bytes() const noexcept {
    return max_response_body_bytes_;
}
std::size_t HttpLimits::max_routes() const noexcept {
    return max_routes_;
}
std::size_t HttpLimits::max_requests_per_dispatch() const noexcept {
    return max_requests_per_dispatch_;
}

}  // namespace iaisf::http
