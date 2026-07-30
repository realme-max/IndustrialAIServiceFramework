#include "iaisf/http/builtin_routes.hpp"

#include <string>
#include <utility>

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

}  // namespace iaisf::http
