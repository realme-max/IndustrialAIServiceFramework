#pragma once

#include <cstddef>
#include <cstdint>

#include "iaisf/core/result.hpp"

namespace iaisf::http {

/**
 * Validated, immutable hard limits shared by the parser, router and serializer.
 *
 * The signed factory rejects negative configuration before conversion. Values
 * are rejected rather than silently clamped.
 */
class HttpLimits final {
public:
    static constexpr std::int64_t kMaximumByteLimit =
        64LL * 1024LL * 1024LL;
    static constexpr std::int64_t kMaximumCountLimit = 1'000'000;

    [[nodiscard]] static Result<HttpLimits> create(
        std::int64_t max_request_line_bytes,
        std::int64_t max_method_bytes,
        std::int64_t max_target_bytes,
        std::int64_t max_header_line_bytes,
        std::int64_t max_header_bytes,
        std::int64_t max_header_count,
        std::int64_t max_body_bytes,
        std::int64_t max_response_body_bytes,
        std::int64_t max_routes,
        std::int64_t max_requests_per_dispatch);

    [[nodiscard]] static HttpLimits defaults() noexcept;

    [[nodiscard]] std::size_t max_request_line_bytes() const noexcept;
    [[nodiscard]] std::size_t max_method_bytes() const noexcept;
    [[nodiscard]] std::size_t max_target_bytes() const noexcept;
    [[nodiscard]] std::size_t max_header_line_bytes() const noexcept;
    [[nodiscard]] std::size_t max_header_bytes() const noexcept;
    [[nodiscard]] std::size_t max_header_count() const noexcept;
    [[nodiscard]] std::size_t max_body_bytes() const noexcept;
    [[nodiscard]] std::size_t max_response_body_bytes() const noexcept;
    [[nodiscard]] std::size_t max_routes() const noexcept;
    [[nodiscard]] std::size_t max_requests_per_dispatch() const noexcept;

private:
    HttpLimits(
        std::size_t max_request_line_bytes,
        std::size_t max_method_bytes,
        std::size_t max_target_bytes,
        std::size_t max_header_line_bytes,
        std::size_t max_header_bytes,
        std::size_t max_header_count,
        std::size_t max_body_bytes,
        std::size_t max_response_body_bytes,
        std::size_t max_routes,
        std::size_t max_requests_per_dispatch) noexcept;

    std::size_t max_request_line_bytes_;
    std::size_t max_method_bytes_;
    std::size_t max_target_bytes_;
    std::size_t max_header_line_bytes_;
    std::size_t max_header_bytes_;
    std::size_t max_header_count_;
    std::size_t max_body_bytes_;
    std::size_t max_response_body_bytes_;
    std::size_t max_routes_;
    std::size_t max_requests_per_dispatch_;
};

}  // namespace iaisf::http
