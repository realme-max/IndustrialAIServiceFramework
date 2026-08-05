#pragma once

#include <ostream>
#include <memory>
#include <string>
#include <vector>

#include "iaisf/logging/async_logger.hpp"
#include "iaisf/metrics/metrics.hpp"

namespace iaisf {

class Application {
public:
    Application(std::ostream& output, std::ostream& error_output);

    /** Runs the command-line application; args excludes executable name. */
    int run(const std::vector<std::string>& args);

    [[nodiscard]] static std::string version_text();
    [[nodiscard]] static std::string usage_text();

private:
    int run_with_config(const std::string& path, bool serve);
    int report_invalid_arguments(const std::string& message);

    std::ostream& output_;
    std::ostream& error_output_;
    // Metrics are application-owned; individual services do not own a global registry.
    MetricsRegistry metrics_registry_;
    // Runtime logging is owned by the application and outlives the service.
    std::unique_ptr<AsyncLogger> runtime_logger_;
};

}  // namespace iaisf
