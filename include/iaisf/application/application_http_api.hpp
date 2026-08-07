#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "iaisf/application/application_contract.hpp"
#include "iaisf/application/application_executor.hpp"
#include "iaisf/application/application_job_clock.hpp"
#include "iaisf/application/application_job_id_generator.hpp"
#include "iaisf/application/application_result_json.hpp"
#include "iaisf/application/application_status_json.hpp"
#include "iaisf/http/http_router.hpp"

namespace iaisf::application {

class ApplicationHttpApi final
    : public std::enable_shared_from_this<ApplicationHttpApi> {
public:
    using Ptr = std::shared_ptr<ApplicationHttpApi>;

    static Result<Ptr> create(
        IApplicationJobRepository& repository,
        ApplicationExecutor& executor,
        IApplicationJobIdGenerator& id_generator,
        const IApplicationJobClock& clock,
        http::HttpLimits limits);

    Result<void> register_routes(http::HttpRouter& router);
    void stop_admission() noexcept;
    bool accepting() const noexcept;

private:
    ApplicationHttpApi(
        IApplicationJobRepository& repository,
        ApplicationExecutor& executor,
        IApplicationJobIdGenerator& id_generator,
        const IApplicationJobClock& clock,
        http::HttpLimits limits) noexcept;

    Result<http::HttpResponse> submit(
        const http::HttpRequest& request,
        IndustrialApplication application,
        ScenePhase phase);
    Result<http::HttpResponse> status(
        const http::HttpRequest& request,
        const std::string& job_id,
        IndustrialApplication application);
    Result<http::HttpResponse> result(
        const http::HttpRequest& request,
        const std::string& job_id,
        IndustrialApplication application);

    IApplicationJobRepository& repository_;
    ApplicationExecutor& executor_;
    IApplicationJobIdGenerator& id_generator_;
    const IApplicationJobClock& clock_;
    http::HttpLimits limits_;
    std::atomic<bool> accepting_{true};
};

}  // namespace iaisf::application
