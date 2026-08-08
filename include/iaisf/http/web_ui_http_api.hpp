#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_response.hpp"
#include "iaisf/http/http_router.hpp"

namespace iaisf::web_ui {

/**
 * Same-origin, compiled-in browser resources for the local application UI.
 *
 * The resource API owns no application state and performs no file I/O.  It is
 * registered during router construction and remains immutable after freeze().
 */
class WebUiHttpApi final
    : public std::enable_shared_from_this<WebUiHttpApi> {
public:
    using Ptr = std::shared_ptr<WebUiHttpApi>;

    [[nodiscard]] static Result<Ptr> create(http::HttpLimits limits);

    [[nodiscard]] Result<void> register_routes(http::HttpRouter& router);

    [[nodiscard]] static constexpr std::size_t route_count() noexcept {
        return 3U;
    }

private:
    struct Resources {
        std::shared_ptr<const http::HttpResponse> html;
        std::shared_ptr<const http::HttpResponse> css;
        std::shared_ptr<const http::HttpResponse> javascript;
    };

    explicit WebUiHttpApi(http::HttpLimits limits) noexcept
        : limits_(std::move(limits)) {}

    [[nodiscard]] Result<void> prepare_resources();
    [[nodiscard]] Result<http::HttpResponse> copy_response(
        const std::shared_ptr<const http::HttpResponse>& resource) const;

    http::HttpLimits limits_;
    Resources resources_;
};

}  // namespace iaisf::web_ui
