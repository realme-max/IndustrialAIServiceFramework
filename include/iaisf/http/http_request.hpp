#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "iaisf/core/result.hpp"

namespace iaisf::http {

struct HttpHeader {
    std::string name;
    std::string value;
};

/**
 * Owning HTTP request value.
 *
 * Header names are lowercase and header lookup returns an owning copy, so a
 * caller cannot retain a view that is invalidated by another object.
 */
class HttpRequest final {
public:
    using Headers = std::vector<HttpHeader>;

    [[nodiscard]] static Result<HttpRequest> create(
        std::string method,
        std::string target,
        Headers headers,
        std::string body,
        bool keep_alive);

    [[nodiscard]] const std::string& method() const noexcept;
    [[nodiscard]] const std::string& target() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept;
    [[nodiscard]] const std::string& query() const noexcept;
    [[nodiscard]] const Headers& headers() const noexcept;
    [[nodiscard]] std::optional<std::string> header(
        const std::string& lowercase_name) const;
    [[nodiscard]] const std::string& body() const noexcept;
    [[nodiscard]] bool keep_alive() const noexcept;

private:
    HttpRequest(
        std::string method,
        std::string target,
        std::string path,
        std::string query,
        Headers headers,
        std::string body,
        bool keep_alive) noexcept;

    std::string method_;
    std::string target_;
    std::string path_;
    std::string query_;
    Headers headers_;
    std::string body_;
    bool keep_alive_;
};

}  // namespace iaisf::http
