#include "iaisf/http/builtin_routes.hpp"

#include <string>
#include <utility>

#include "iaisf/metrics/prometheus_formatter.hpp"
#include "iaisf/http/health_routes.hpp"
#include "iaisf/version.hpp"

namespace iaisf::http {
namespace {

Result<HttpResponse> health_handler(const HttpRequest&) {
    HttpResponse response{HttpStatus::Ok};
    auto header =
        response.set_header("Content-Type", "application/json");
    if (!header) {
        return Result<HttpResponse>::failure(std::move(header).error());
    }
    response.set_body("{\"status\":\"ok\"}");
    return Result<HttpResponse>::success(std::move(response));
}

Result<HttpResponse> version_handler(const HttpRequest&) {
    HttpResponse response{HttpStatus::Ok};
    auto header =
        response.set_header("Content-Type", "application/json");
    if (!header) {
        return Result<HttpResponse>::failure(std::move(header).error());
    }
    std::string body{
        "{\"name\":\"IndustrialAIServiceFramework\",\"version\":\""};
    body.append(kVersionString);
    body.append("\"}");
    response.set_body(std::move(body));
    return Result<HttpResponse>::success(std::move(response));
}

Result<HttpResponse> metrics_handler(
    MetricsRegistry* const metrics,
    const HttpRequest&) {
    if (metrics == nullptr) {
        return Result<HttpResponse>::failure(make_error(
            ErrorCode::InvalidState,
            "metrics registry is unavailable"));
    }
    auto snapshot = metrics->snapshot();
    if (!snapshot) {
        return Result<HttpResponse>::failure(std::move(snapshot).error());
    }
    auto formatted = metrics::PrometheusFormatter::format(snapshot.value());
    if (!formatted) {
        return Result<HttpResponse>::failure(std::move(formatted).error());
    }
    HttpResponse response{HttpStatus::Ok};
    auto header = response.set_header(
        "Content-Type", "text/plain; version=0.0.4");
    if (!header) {
        return Result<HttpResponse>::failure(std::move(header).error());
    }
    response.set_body(std::move(formatted).value());
    return Result<HttpResponse>::success(std::move(response));
}

}  // namespace

Result<void> register_builtin_routes(HttpRouter& router) {
    auto health = router.add_route("GET", "/health", health_handler);
    if (!health) {
        return health;
    }
    auto version = router.add_route("GET", "/version", version_handler);
    if (!version) {
        return version;
    }
    return Result<void>::success();
}

Result<void> register_builtin_routes(
    HttpRouter& router,
    std::weak_ptr<const health::HealthChecker> checker) {
    auto health = register_health_routes(router, std::move(checker));
    if (!health) {
        return health;
    }
    return router.add_route("GET", "/version", version_handler);
}

Result<void> register_metrics_route(
    HttpRouter& router,
    MetricsRegistry& metrics,
    std::string endpoint) {
    if (endpoint.empty()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "metrics endpoint must not be empty"));
    }
    return router.add_route(
        "GET",
        std::move(endpoint),
        [&metrics](const HttpRequest& request) {
            return metrics_handler(&metrics, request);
        });
}

}  // namespace iaisf::http
