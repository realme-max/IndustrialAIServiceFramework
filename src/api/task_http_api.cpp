#include "iaisf/api/task_http_api.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "iaisf/api/task_snapshot_json.hpp"
#include "iaisf/core/error.hpp"
#include "iaisf/http/http_status.hpp"

namespace iaisf::api {
namespace {

using http::HttpResponse;
using http::HttpStatus;

std::string trim_ows(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1U);
    }
    return std::string(value);
}

std::string ascii_lower(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const char character) {
            const auto byte = static_cast<unsigned char>(character);
            return static_cast<char>(std::tolower(byte));
        });
    return value;
}

bool is_json_content_type(const std::string& raw) {
    const auto semicolon = raw.find(';');
    const std::string media = ascii_lower(trim_ows(
        std::string_view{raw}.substr(0U, semicolon)));
    if (media != "application/json") {
        return false;
    }
    if (semicolon == std::string::npos) {
        return true;
    }
    std::string_view parameters{raw};
    parameters.remove_prefix(semicolon + 1U);
    const auto extra_semicolon = parameters.find(';');
    if (extra_semicolon != std::string_view::npos) {
        return false;
    }
    const auto equals = parameters.find('=');
    if (equals == std::string_view::npos) {
        return false;
    }
    const std::string name = ascii_lower(trim_ows(parameters.substr(0U, equals)));
    std::string value = ascii_lower(trim_ows(parameters.substr(equals + 1U)));
    if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
        value = value.substr(1U, value.size() - 2U);
    }
    return name == "charset" && value == "utf-8";
}

Result<HttpResponse> json_response(
    const HttpStatus status,
    const nlohmann::json& body,
    const http::HttpLimits& limits) {
    try {
        std::string serialized = body.dump();
        if (serialized.size() > limits.max_response_body_bytes()) {
            return Result<HttpResponse>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "task API response exceeds the HTTP body limit"));
        }
        HttpResponse response{status};
        auto header = response.set_header(
            "Content-Type",
            "application/json; charset=utf-8");
        if (!header) {
            return Result<HttpResponse>::failure(std::move(header).error());
        }
        response.set_body(std::move(serialized));
        return Result<HttpResponse>::success(std::move(response));
    } catch (const std::bad_alloc&) {
        return Result<HttpResponse>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate task API response"));
    } catch (const std::exception&) {
        return Result<HttpResponse>::failure(make_error(
            ErrorCode::InternalError,
            "unable to serialize task API response"));
    }
}

Result<HttpResponse> api_error(
    const HttpStatus status,
    std::string_view code,
    std::string_view message,
    const http::HttpLimits& limits) {
    return json_response(
        status,
        nlohmann::json{
            {"error", {{"code", code}, {"message", message}}}},
        limits);
}

}  // namespace

Result<TaskHttpApi::Ptr> TaskHttpApi::create(
    task::TaskManager& task_manager,
    const plugin::PluginRuntime& plugin_runtime,
    task::TaskLimits task_limits,
    http::HttpLimits http_limits,
    TaskApiLimits api_limits) {
    if (!plugin_runtime.frozen()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task API requires a frozen plugin runtime"));
    }
    auto capacity = validate_task_api_capacity(
        task_limits,
        plugin_runtime.limits(),
        http_limits,
        api_limits);
    if (!capacity) {
        return Result<Ptr>::failure(std::move(capacity).error());
    }
    try {
        return Result<Ptr>::success(std::shared_ptr<TaskHttpApi>{
            new TaskHttpApi{
                ConstructionKey{},
                task_manager,
                plugin_runtime,
                std::move(task_limits),
                std::move(http_limits),
                std::move(api_limits)}});
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate task HTTP API"));
    }
}

Result<TaskHttpApi::Ptr> TaskHttpApi::create(
    task::TaskManager& task_manager,
    const plugin::PluginManager& plugin_manager,
    task::TaskLimits task_limits,
    http::HttpLimits http_limits,
    TaskApiLimits api_limits) {
    if (!plugin_manager.frozen()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "task API requires a frozen plugin manager"));
    }
    auto capacity = validate_task_api_capacity(
        task_limits,
        plugin_manager.limits(),
        http_limits,
        api_limits);
    if (!capacity) {
        return Result<Ptr>::failure(std::move(capacity).error());
    }
    try {
        return Result<Ptr>::success(std::shared_ptr<TaskHttpApi>{
            new TaskHttpApi{
                ConstructionKey{},
                task_manager,
                plugin_manager,
                std::move(task_limits),
                std::move(http_limits),
                std::move(api_limits)}});
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate task HTTP API"));
    }
}

TaskHttpApi::TaskHttpApi(
    ConstructionKey,
    task::TaskManager& task_manager,
    const plugin::PluginRuntime& plugin_runtime,
    task::TaskLimits task_limits,
    http::HttpLimits http_limits,
    TaskApiLimits api_limits) noexcept
    : task_manager_(task_manager),
      plugin_runtime_(&plugin_runtime),
      task_limits_(std::move(task_limits)),
      http_limits_(std::move(http_limits)),
      api_limits_(std::move(api_limits)) {}

TaskHttpApi::TaskHttpApi(
    ConstructionKey,
    task::TaskManager& task_manager,
    const plugin::PluginManager& plugin_manager,
    task::TaskLimits task_limits,
    http::HttpLimits http_limits,
    TaskApiLimits api_limits) noexcept
    : task_manager_(task_manager),
      plugin_manager_(&plugin_manager),
      task_limits_(std::move(task_limits)),
      http_limits_(std::move(http_limits)),
      api_limits_(std::move(api_limits)) {}

Result<void> TaskHttpApi::register_routes(http::HttpRouter& router) {
    const std::weak_ptr<TaskHttpApi> weak = weak_from_this();
    auto post = router.add_route(
        "POST",
        "/v1/tasks",
        [weak](const http::HttpRequest& request) {
            const auto api = weak.lock();
            if (!api) {
                return Result<HttpResponse>::success(
                    HttpResponse::error(HttpStatus::ServiceUnavailable, true));
            }
            return api->submit_task(request);
        });
    if (!post) {
        return post;
    }
    auto get = router.add_terminal_parameter_route(
        "GET",
        "/v1/tasks/",
        [weak](
            const http::HttpRequest& request,
            const std::string& task_id) {
            const auto api = weak.lock();
            if (!api) {
                return Result<HttpResponse>::success(
                    HttpResponse::error(HttpStatus::ServiceUnavailable, true));
            }
            return api->get_task(request, task_id);
        });
    if (!get) {
        return get;
    }
    return router.set_routing_error_handler(
        [weak](const HttpStatus status, const http::HttpRequest& request) {
            const auto api = weak.lock();
            if (!api) {
                return Result<HttpResponse>::success(
                    HttpResponse::error(HttpStatus::ServiceUnavailable, true));
            }
            return api->routing_error(status, request);
        });
}

void TaskHttpApi::stop_admission() noexcept {
    accepting_submissions_.store(false, std::memory_order_release);
}

bool TaskHttpApi::accepting_submissions() const noexcept {
    return accepting_submissions_.load(std::memory_order_acquire);
}

Result<HttpResponse> TaskHttpApi::submit_task(
    const http::HttpRequest& request) {
    if (!accepting_submissions()) {
        return api_error(
            HttpStatus::ServiceUnavailable,
            "service_stopping",
            "service temporarily unavailable",
            http_limits_);
    }
    const auto content_type = request.header("content-type");
    if (!content_type.has_value() || !is_json_content_type(*content_type)) {
        return api_error(
            HttpStatus::UnsupportedMediaType,
            "unsupported_media_type",
            "content type must be application/json",
            http_limits_);
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(
            request.body(),
            nullptr,
            false,
            false);
    } catch (...) {
        document = nlohmann::json::value_t::discarded;
    }
    if (document.is_discarded() || !document.is_object() ||
        document.size() != 2U ||
        !document.contains("operation") ||
        !document.contains("input") ||
        !document["operation"].is_string()) {
        return api_error(
            HttpStatus::BadRequest,
            "invalid_request",
            "request body must contain only operation and input",
            http_limits_);
    }

    task::TaskRequest task_request;
    try {
        task_request.operation =
            document["operation"].get<std::string>();
        task_request.input = std::move(document["input"]);
    } catch (...) {
        return api_error(
            HttpStatus::BadRequest,
            "invalid_request",
            "request JSON values are invalid",
            http_limits_);
    }

    auto request_capacity = task_limits_.validate_request(task_request);
    if (!request_capacity) {
        return api_error(
            request_capacity.error().code == ErrorCode::ResourceExhausted
                ? HttpStatus::PayloadTooLarge
                : HttpStatus::BadRequest,
            request_capacity.error().code == ErrorCode::ResourceExhausted
                ? "payload_too_large"
                : "invalid_request",
            request_capacity.error().code == ErrorCode::ResourceExhausted
                ? "task input exceeds configured limits"
                : "task request is invalid",
            http_limits_);
    }

    auto plugin_validation = plugin_runtime_ != nullptr
                                 ? plugin_runtime_->validate(
                                       task_request.operation,
                                       task_request.input)
                                 : plugin_manager_->validate(
                                       task_request.operation,
                                       task_request.input);
    if (!plugin_validation) {
        if (plugin_validation.error().code == ErrorCode::NotFound) {
            return api_error(
                HttpStatus::NotFound,
                "operation_not_found",
                "requested operation was not found",
                http_limits_);
        }
        if (plugin_validation.error().code == ErrorCode::InvalidArgument ||
            plugin_validation.error().code == ErrorCode::ResourceExhausted) {
            return api_error(
                HttpStatus::UnprocessableContent,
                "validation_failed",
                "plugin input validation failed",
                http_limits_);
        }
        return api_error(
            HttpStatus::InternalServerError,
            "internal_error",
            "internal service error",
            http_limits_);
    }

    // Validation may invoke user plugin code. A concurrent service stop can
    // close admission while validation runs, so recheck before entering the
    // TaskManager submission transaction.
    if (!accepting_submissions()) {
        return api_error(
            HttpStatus::ServiceUnavailable,
            "service_stopping",
            "service temporarily unavailable",
            http_limits_);
    }

    auto submitted = task_manager_.submit_with_outcome(task_request);
    if (!submitted.result) {
        if (submitted.failure == task::TaskSubmitFailure::NotAccepting) {
            return api_error(
                HttpStatus::ServiceUnavailable,
                "service_stopping",
                "service temporarily unavailable",
                http_limits_);
        }
        if (submitted.failure == task::TaskSubmitFailure::QueueCapacity) {
            return api_error(
                HttpStatus::ServiceUnavailable,
                "queue_full",
                "service temporarily unavailable",
                http_limits_);
        }
        if (submitted.failure ==
            task::TaskSubmitFailure::RepositoryCapacity) {
            return api_error(
                HttpStatus::ServiceUnavailable,
                "repository_full",
                "service temporarily unavailable",
                http_limits_);
        }
        if (submitted.failure ==
            task::TaskSubmitFailure::ResourceFailure) {
            return api_error(
                HttpStatus::ServiceUnavailable,
                "capacity_exhausted",
                "service temporarily unavailable",
                http_limits_);
        }
        return api_error(
            HttpStatus::InternalServerError,
            "internal_error",
            "internal service error",
            http_limits_);
    }

    const std::string id = submitted.result.value().to_string();
    std::string status_url = "/v1/tasks/" + id;
    if (status_url.size() > api_limits_.max_status_url_bytes()) {
        return api_error(
            HttpStatus::InternalServerError,
            "internal_error",
            "internal service error",
            http_limits_);
    }
    return json_response(
        HttpStatus::Accepted,
        nlohmann::json{
            {"task_id", id},
            {"status_url", std::move(status_url)}},
        http_limits_);
}

Result<HttpResponse> TaskHttpApi::get_task(
    const http::HttpRequest&,
    const std::string& task_id_text) {
    auto id = task::TaskId::parse(task_id_text);
    if (!id) {
        return api_error(
            HttpStatus::BadRequest,
            "invalid_task_id",
            "task id is invalid",
            http_limits_);
    }
    auto snapshot = task_manager_.get_snapshot(id.value());
    if (!snapshot) {
        if (snapshot.error().code == ErrorCode::NotFound) {
            return api_error(
                HttpStatus::NotFound,
                "task_not_found",
                "task was not found",
                http_limits_);
        }
        return api_error(
            HttpStatus::InternalServerError,
            "internal_error",
            "internal service error",
            http_limits_);
    }
    auto view = task_snapshot_to_json(snapshot.value());
    if (!view) {
        return api_error(
            HttpStatus::InternalServerError,
            "internal_error",
            "internal service error",
            http_limits_);
    }
    return json_response(HttpStatus::Ok, view.value(), http_limits_);
}

Result<HttpResponse> TaskHttpApi::routing_error(
    const HttpStatus status,
    const http::HttpRequest&) {
    if (status == HttpStatus::MethodNotAllowed) {
        return api_error(
            status,
            "method_not_allowed",
            "method is not allowed for this resource",
            http_limits_);
    }
    return api_error(
        HttpStatus::NotFound,
        "route_not_found",
        "requested route was not found",
        http_limits_);
}

}  // namespace iaisf::api
