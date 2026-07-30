#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_request.hpp"
#include "iaisf/http/http_status.hpp"

namespace iaisf::http {

/**
 * Bounded HTTP/1.1 response builder.
 *
 * Content-Length and Connection are framework-owned headers. Serialization is
 * binary-safe and validates every user header against response splitting.
 */
class HttpResponse final {
public:
    explicit HttpResponse(HttpStatus status = HttpStatus::Ok) noexcept;

    [[nodiscard]] HttpStatus status() const noexcept;
    [[nodiscard]] const std::vector<HttpHeader>& headers() const noexcept;
    [[nodiscard]] const std::string& body() const noexcept;
    [[nodiscard]] bool close_connection() const noexcept;

    [[nodiscard]] Result<void> set_header(
        std::string name,
        std::string value);
    void set_body(std::string body) noexcept;
    void set_close_connection(bool close) noexcept;

    [[nodiscard]] Result<void> validate(const HttpLimits& limits) const;
    [[nodiscard]] Result<std::string> serialize(
        const HttpLimits& limits) const;

    [[nodiscard]] static HttpResponse error(
        HttpStatus status,
        bool close_connection = true);

private:
    HttpStatus status_;
    std::vector<HttpHeader> headers_;
    std::string body_;
    bool close_connection_{false};
};

}  // namespace iaisf::http
