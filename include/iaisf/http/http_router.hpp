#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_request.hpp"
#include "iaisf/http/http_response.hpp"

namespace iaisf::http {

/**
 * Exact method/path router.
 *
 * Registration is owner-thread-only. freeze() makes the route table immutable
 * for request dispatch. Handler failures and exceptions become a closed 500
 * response without exposing internal diagnostics.
 */
class HttpRouter final {
public:
    using Handler = std::function<Result<HttpResponse>(const HttpRequest&)>;

    explicit HttpRouter(HttpLimits limits = HttpLimits::defaults());

    HttpRouter(const HttpRouter&) = delete;
    HttpRouter& operator=(const HttpRouter&) = delete;
    HttpRouter(HttpRouter&&) noexcept = default;
    HttpRouter& operator=(HttpRouter&&) noexcept = default;
    ~HttpRouter() = default;

    [[nodiscard]] Result<void> add_route(
        std::string method,
        std::string path,
        Handler handler);
    [[nodiscard]] Result<void> freeze();
    [[nodiscard]] Result<HttpResponse> dispatch(
        const HttpRequest& request) const;

    [[nodiscard]] bool frozen() const noexcept;
    [[nodiscard]] std::size_t route_count() const noexcept;

private:
    struct Route {
        std::string method;
        std::string path;
        Handler handler;
    };

    HttpLimits limits_;
    std::vector<Route> routes_;
    bool frozen_{false};
};

}  // namespace iaisf::http
