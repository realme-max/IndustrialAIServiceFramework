#pragma once

#include <memory>
#include <string>

#include "iaisf/core/result.hpp"
#include "iaisf/health/health_checker.hpp"
#include "iaisf/http/http_router.hpp"
#include "iaisf/metrics/metrics.hpp"

namespace iaisf::http {

[[nodiscard]] Result<void> register_builtin_routes(HttpRouter& router);

/** Registers production lifecycle health routes plus /version. */
[[nodiscard]] Result<void> register_builtin_routes(
    HttpRouter& router,
    std::weak_ptr<const health::HealthChecker> checker);

/** Registers a read-only Prometheus metrics endpoint backed by a snapshot. */
[[nodiscard]] Result<void> register_metrics_route(
    HttpRouter& router,
    MetricsRegistry& metrics,
    std::string endpoint);

}  // namespace iaisf::http
