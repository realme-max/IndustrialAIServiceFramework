#pragma once

#include <string>

#include "iaisf/core/result.hpp"
#include "iaisf/metrics/metrics.hpp"

namespace iaisf::metrics {

/** Formats a point-in-time registry snapshot as Prometheus text. */
class PrometheusFormatter final {
public:
    [[nodiscard]] static Result<std::string> format(
        const MetricsSnapshot& snapshot);
};

}  // namespace iaisf::metrics
