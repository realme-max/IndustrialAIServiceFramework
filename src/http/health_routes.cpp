#include "iaisf/http/health_routes.hpp"

#include <string>
#include <utility>

#include "iaisf/http/http_response.hpp"
#include "iaisf/http/http_status.hpp"

namespace iaisf::http {
namespace {

HttpResponse response_for(
    const std::weak_ptr<const health::HealthChecker>& weak_checker,
    const bool readiness) {
    const auto checker = weak_checker.lock();
    if (!checker) {
        auto response = HttpResponse{HttpStatus::ServiceUnavailable};
        if (!response.set_header(
                "Content-Type", "application/json; charset=utf-8")) {
            return HttpResponse::error(HttpStatus::ServiceUnavailable, true);
        }
        response.set_body(
            "{\"status\":\"unavailable\",\"live\":false,"
            "\"ready\":false,\"phase\":\"stopped\"}");
        response.set_close_connection(true);
        return response;
    }

    const auto snapshot = checker->snapshot();
    const bool ok = readiness ? snapshot.ready : snapshot.live;
    const auto status = readiness
        ? (ok ? "ready" : "not_ready")
        : (ok ? "ok" : "stopped");
    std::string body;
    body.reserve(96U);
    body.append("{\"status\":\"");
    body.append(status);
    body.append("\",\"live\":");
    body.append(snapshot.live ? "true" : "false");
    body.append(",\"ready\":");
    body.append(snapshot.ready ? "true" : "false");
    body.append(",\"phase\":\"");
    body.append(health::to_string(snapshot.phase));
    body.append("\"}");

    auto response = HttpResponse{ok ? HttpStatus::Ok
                                    : HttpStatus::ServiceUnavailable};
    if (!response.set_header("Content-Type", "application/json; charset=utf-8")) {
        return HttpResponse::error(HttpStatus::ServiceUnavailable, true);
    }
    response.set_body(std::move(body));
    response.set_close_connection(false);
    return response;
}

}  // namespace

Result<void> register_health_routes(
    HttpRouter& router,
    std::weak_ptr<const health::HealthChecker> checker) {
    auto health = router.add_route(
        "GET",
        "/health",
        [checker](const HttpRequest&) {
            return Result<HttpResponse>::success(response_for(checker, false));
        });
    if (!health) {
        return health;
    }
    return router.add_route(
        "GET",
        "/ready",
        [checker](const HttpRequest&) {
            return Result<HttpResponse>::success(response_for(checker, true));
        });
}

}  // namespace iaisf::http
