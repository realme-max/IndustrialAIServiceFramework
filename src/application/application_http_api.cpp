#include "iaisf/application/application_http_api.hpp"

#include <new>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "iaisf/core/error.hpp"

namespace iaisf::application {
namespace {

using http::HttpResponse;
using http::HttpStatus;

Result<HttpResponse> json_response(
    const HttpStatus status,
    nlohmann::ordered_json body,
    const http::HttpLimits& limits) {
    try {
        auto serialized = body.dump();
        if (serialized.size() > limits.max_response_body_bytes()) {
            return Result<HttpResponse>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "application response exceeds the HTTP body limit"));
        }
        HttpResponse response{status};
        auto header = response.set_header(
            "Content-Type", "application/json; charset=utf-8");
        if (!header) {
            return Result<HttpResponse>::failure(std::move(header).error());
        }
        response.set_body(std::move(serialized));
        return Result<HttpResponse>::success(std::move(response));
    } catch (const std::bad_alloc&) {
        return Result<HttpResponse>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate application HTTP response"));
    } catch (...) {
        return Result<HttpResponse>::failure(make_error(
            ErrorCode::InternalError,
            "unable to serialize application HTTP response"));
    }
}

Result<HttpResponse> api_error(
    const HttpStatus status,
    std::string_view code,
    std::string_view message,
    const http::HttpLimits& limits) {
    return json_response(
        status,
        nlohmann::ordered_json{
            {"error", {{"code", code}, {"message", message}}}},
        limits);
}

bool json_content_type(const std::optional<std::string>& value) {
    if (!value.has_value()) {
        return false;
    }
    const auto semicolon = value->find(';');
    return value->substr(0U, semicolon) == "application/json";
}

HttpStatus contract_status(const ApplicationContractErrorCategory category) {
    switch (category) {
        case ApplicationContractErrorCategory::InvalidJson:
        case ApplicationContractErrorCategory::InvalidRequest:
            return HttpStatus::BadRequest;
        case ApplicationContractErrorCategory::ValidationFailed:
            return HttpStatus::UnprocessableContent;
        case ApplicationContractErrorCategory::PayloadTooLarge:
            return HttpStatus::PayloadTooLarge;
        case ApplicationContractErrorCategory::ResourceFailure:
            return HttpStatus::ServiceUnavailable;
        case ApplicationContractErrorCategory::InternalFailure:
            return HttpStatus::InternalServerError;
    }
    return HttpStatus::InternalServerError;
}

}  // namespace

ApplicationHttpApi::ApplicationHttpApi(
    IApplicationJobRepository& repository,
    ApplicationExecutor& executor,
    IApplicationJobIdGenerator& id_generator,
    const IApplicationJobClock& clock,
    http::HttpLimits limits) noexcept
    : repository_(repository), executor_(executor),
      id_generator_(id_generator), clock_(clock), limits_(std::move(limits)) {}

Result<ApplicationHttpApi::Ptr> ApplicationHttpApi::create(
    IApplicationJobRepository& repository,
    ApplicationExecutor& executor,
    IApplicationJobIdGenerator& id_generator,
    const IApplicationJobClock& clock,
    http::HttpLimits limits) {
    try {
        return Result<Ptr>::success(std::shared_ptr<ApplicationHttpApi>{
            new ApplicationHttpApi{repository, executor, id_generator, clock,
                                   std::move(limits)}});
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate application HTTP API"));
    }
}

Result<void> ApplicationHttpApi::register_routes(http::HttpRouter& router) {
    const auto weak = weak_from_this();
    auto add_post = [&](const char* path, const IndustrialApplication app,
                        const ScenePhase phase) -> Result<void> {
        return router.add_route(
            "POST", path,
            [weak, app, phase](const http::HttpRequest& request) {
                const auto api = weak.lock();
                if (!api) {
                    return Result<HttpResponse>::success(
                        HttpResponse::error(HttpStatus::ServiceUnavailable, true));
                }
                return api->submit(request, app, phase);
            });
    };
    auto result = add_post("/api/weld-inspection/v1/jobs",
                           IndustrialApplication::WeldInspection,
                           ScenePhase::PostWeld);
    if (!result) return result;
    result = add_post("/api/welding-guidance/v1/jobs",
                      IndustrialApplication::WeldingGuidance,
                      ScenePhase::PreWeld);
    if (!result) return result;

    auto add_get = [&](const char* prefix, const IndustrialApplication app,
                       const bool result_route) -> Result<void> {
        return router.add_terminal_parameter_route(
            "GET", prefix,
            [weak, app, result_route](const http::HttpRequest& request,
                                      const std::string& id) {
                const auto api = weak.lock();
                if (!api) {
                    return Result<HttpResponse>::success(
                        HttpResponse::error(HttpStatus::ServiceUnavailable, true));
                }
                return result_route ? api->result(request, id, app)
                                    : api->status(request, id, app);
            });
    };
    result = add_get("/api/weld-inspection/v1/jobs/",
                     IndustrialApplication::WeldInspection, false);
    if (!result) return result;
    result = add_get("/api/weld-inspection/v1/results/",
                     IndustrialApplication::WeldInspection, true);
    if (!result) return result;
    result = add_get("/api/welding-guidance/v1/jobs/",
                     IndustrialApplication::WeldingGuidance, false);
    if (!result) return result;
    return add_get("/api/welding-guidance/v1/results/",
                   IndustrialApplication::WeldingGuidance, true);
}

void ApplicationHttpApi::stop_admission() noexcept {
    accepting_.store(false, std::memory_order_release);
    executor_.stop_admission();
}

bool ApplicationHttpApi::accepting() const noexcept {
    return accepting_.load(std::memory_order_acquire);
}

Result<HttpResponse> ApplicationHttpApi::submit(
    const http::HttpRequest& request,
    const IndustrialApplication application,
    const ScenePhase phase) {
    if (!accepting()) {
        return api_error(HttpStatus::ServiceUnavailable, "service_stopping",
                         "service temporarily unavailable", limits_);
    }
    if (!json_content_type(request.header("content-type"))) {
        return api_error(HttpStatus::UnsupportedMediaType,
                         "unsupported_media_type",
                         "content type must be application/json", limits_);
    }
    const auto contract = application == IndustrialApplication::WeldInspection
                              ? parse_weld_inspection_submit(request.body())
                              : parse_welding_guidance_submit(request.body());
    if (!contract) {
        return api_error(contract_status(contract.error().category),
                         to_string(contract.error().category),
                         "application request is invalid", limits_);
    }
    auto now = clock_.now();
    if (!now) {
        return api_error(HttpStatus::InternalServerError, "internal_error",
                         "internal service error", limits_);
    }

    for (unsigned int attempt = 0U; attempt < 3U; ++attempt) {
        auto generated = id_generator_.generate(application);
        if (!generated) {
            return api_error(HttpStatus::ServiceUnavailable,
                             "id_generation_failed", "service unavailable",
                             limits_);
        }
        ApplicationJobId id = generated.value();
        ApplicationJobCreateRequest create_request{
            id, application, phase, contract.value().submission, now.value(),
            {contract.value().input_artifact}};
        auto created = repository_.create(create_request);
        if (!created) {
            if (created.error().category ==
                ApplicationRepositoryFailure::DuplicateId) {
                continue;
            }
            const auto status = created.error().category ==
                                        ApplicationRepositoryFailure::CapacityExceeded
                                    ? HttpStatus::ServiceUnavailable
                                    : HttpStatus::UnprocessableContent;
            return api_error(status, "job_rejected", "application job rejected",
                             limits_);
        }
        auto queued = repository_.transition(
            id, application, created.value().version(),
            ApplicationJobState::Queued, now.value());
        if (!queued) {
            auto failed = repository_.transition(
                id, application, created.value().version(),
                ApplicationJobState::Failed, now.value());
            if (failed) {
                (void)repository_.erase_terminal(
                    id, application, failed.value().version());
            }
            return api_error(HttpStatus::InternalServerError, "internal_error",
                             "internal service error", limits_);
        }
        auto submitted = executor_.submit(id);
        if (!submitted) {
            auto failed = repository_.transition(
                id, application, queued.value().version(),
                ApplicationJobState::Failed, now.value());
            if (failed) {
                (void)repository_.erase_terminal(
                    id, application, failed.value().version());
            }
            return api_error(HttpStatus::ServiceUnavailable, "queue_full",
                             "service temporarily unavailable", limits_);
        }
        const std::string status_url =
            application == IndustrialApplication::WeldInspection
                ? "/api/weld-inspection/v1/jobs/" + std::string{id.value()}
                : "/api/welding-guidance/v1/jobs/" + std::string{id.value()};
        return json_response(
            HttpStatus::Accepted,
            nlohmann::ordered_json{{"job_id", std::string{id.value()}},
                                   {"status_url", status_url}},
            limits_);
    }
    return api_error(HttpStatus::ServiceUnavailable, "id_collision",
                     "unable to allocate a unique job id", limits_);
}

Result<HttpResponse> ApplicationHttpApi::status(
    const http::HttpRequest&, const std::string& job_id,
    const IndustrialApplication application) {
    auto parsed = ApplicationJobId::parse(job_id);
    if (!parsed) {
        return api_error(HttpStatus::BadRequest, "invalid_job_id",
                         "job id is invalid", limits_);
    }
    auto snapshot = repository_.get(parsed.value(), application);
    if (!snapshot) {
        return api_error(HttpStatus::NotFound, "job_not_found",
                         "application job was not found", limits_);
    }
    auto body = application_job_status_json(snapshot.value());
    if (!body) {
        return api_error(HttpStatus::InternalServerError, "internal_error",
                         "internal service error", limits_);
    }
    HttpResponse response{HttpStatus::Ok};
    auto header = response.set_header(
        "Content-Type", "application/json; charset=utf-8");
    if (!header) return Result<HttpResponse>::failure(std::move(header).error());
    response.set_body(std::move(body).value());
    return Result<HttpResponse>::success(std::move(response));
}

Result<HttpResponse> ApplicationHttpApi::result(
    const http::HttpRequest&, const std::string& job_id,
    const IndustrialApplication application) {
    auto parsed = ApplicationJobId::parse(job_id);
    if (!parsed) {
        return api_error(HttpStatus::BadRequest, "invalid_job_id",
                         "job id is invalid", limits_);
    }
    auto snapshot = repository_.get(parsed.value(), application);
    if (!snapshot) {
        return api_error(HttpStatus::NotFound, "job_not_found",
                         "application job was not found", limits_);
    }
    const auto state = snapshot.value().state();
    if (state != ApplicationJobState::Succeeded &&
        state != ApplicationJobState::WaitingHuman) {
        const bool terminal = is_terminal(state);
        return api_error(HttpStatus::Conflict,
                         terminal ? "job_not_succeeded" : "result_not_ready",
                         terminal ? "application job did not succeed"
                                  : "application result is not ready",
                         limits_);
    }
    auto body = application_execution_result_json(snapshot.value());
    if (!body) {
        return api_error(HttpStatus::InternalServerError, "internal_error",
                         "internal service error", limits_);
    }
    HttpResponse response{HttpStatus::Ok};
    auto header = response.set_header(
        "Content-Type", "application/json; charset=utf-8");
    if (!header) return Result<HttpResponse>::failure(std::move(header).error());
    response.set_body(std::move(body).value());
    return Result<HttpResponse>::success(std::move(response));
}

}  // namespace iaisf::application
