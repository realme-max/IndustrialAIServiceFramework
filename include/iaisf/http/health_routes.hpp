#pragma once

#include <memory>

#include "iaisf/core/result.hpp"
#include "iaisf/health/health_checker.hpp"
#include "iaisf/http/http_router.hpp"

namespace iaisf::http {

[[nodiscard]] Result<void> register_health_routes(
    HttpRouter& router,
    std::weak_ptr<const health::HealthChecker> checker);

}  // namespace iaisf::http
