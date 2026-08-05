#include "iaisf/http/diagnostics_routes.hpp"

#include <utility>

namespace iaisf::http {

Result<void> register_diagnostics_route(
    HttpRouter& router,
    std::weak_ptr<const diagnostics::RuntimeDiagnostics> diagnostics,
    std::string endpoint,
    const std::size_t maximum_response_bytes) {
    if (endpoint.empty() || endpoint.front() != '/') {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "diagnostics endpoint must be a valid path"));
    }
    return router.add_route(
        "GET", std::move(endpoint),
        [diagnostics = std::move(diagnostics), maximum_response_bytes](
            const HttpRequest&) -> Result<HttpResponse> {
            const auto object = diagnostics.lock();
            if (!object) {
                HttpResponse response{HttpStatus::ServiceUnavailable};
                (void)response.set_header("Content-Type",
                                          "application/json; charset=utf-8");
                (void)response.set_header("Cache-Control", "no-store");
                response.set_close_connection(true);
                response.set_body("{\"status\":\"unavailable\"}");
                return Result<HttpResponse>::success(std::move(response));
            }
            auto snapshot = object->snapshot();
            if (!snapshot) {
                HttpResponse response{HttpStatus::ServiceUnavailable};
                (void)response.set_header("Content-Type",
                                          "application/json; charset=utf-8");
                (void)response.set_header("Cache-Control", "no-store");
                response.set_close_connection(true);
                response.set_body("{\"status\":\"unavailable\"}");
                return Result<HttpResponse>::success(std::move(response));
            }
            if (!snapshot.value().health.live) {
                HttpResponse response{HttpStatus::ServiceUnavailable};
                (void)response.set_header("Content-Type",
                                          "application/json; charset=utf-8");
                (void)response.set_header("Cache-Control", "no-store");
                response.set_close_connection(true);
                response.set_body("{\"status\":\"stopped\"}");
                return Result<HttpResponse>::success(std::move(response));
            }
            auto body = diagnostics::to_json(snapshot.value(),
                                              maximum_response_bytes);
            if (!body) {
                HttpResponse response{HttpStatus::ServiceUnavailable};
                (void)response.set_header("Content-Type",
                                          "application/json; charset=utf-8");
                (void)response.set_header("Cache-Control", "no-store");
                response.set_close_connection(true);
                response.set_body("{\"status\":\"unavailable\"}");
                return Result<HttpResponse>::success(std::move(response));
            }
            HttpResponse response{HttpStatus::Ok};
            auto content_type = response.set_header(
                "Content-Type", "application/json; charset=utf-8");
            if (!content_type) {
                return Result<HttpResponse>::failure(std::move(content_type).error());
            }
            auto cache = response.set_header("Cache-Control", "no-store");
            if (!cache) {
                return Result<HttpResponse>::failure(std::move(cache).error());
            }
            response.set_body(std::move(body).value());
            return Result<HttpResponse>::success(std::move(response));
        });
}

} // namespace iaisf::http
