#include "iaisf/app/application.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "iaisf/config/app_config.hpp"
#include "iaisf/core/error.hpp"
#include "iaisf/logging/console_logger.hpp"
#include "iaisf/version.hpp"

#if defined(IAISF_HAS_SERVICE_RUNTIME)
#include "iaisf/net/event_loop.hpp"
#include "iaisf/service/industrial_ai_service.hpp"
#include "iaisf/service/runtime_options.hpp"
#endif

namespace iaisf {
namespace {

constexpr int kSuccessExitCode = 0;
constexpr int kConfigurationExitCode = 1;
constexpr int kUsageExitCode = 2;

void report_failure(std::ostream &output, const std::string_view category,
                    const Error &error) {
    output << category << " failed [" << to_string(error.code)
           << "]: " << error.message << '\n';
}

} // namespace

Application::Application(std::ostream &output, std::ostream &error_output)
    : output_(output), error_output_(error_output) {}

int Application::run(const std::vector<std::string> &args) {
    if (args.empty()) {
        output_
            << version_text() << '\n'
            << "No service mode was selected; use --serve --config <path>.\n";
        return kSuccessExitCode;
    }
    if (args.size() == 1U && args.front() == "--version") {
        output_ << version_text() << '\n';
        return kSuccessExitCode;
    }
    if (args.size() == 1U && args.front() == "--help") {
        output_ << usage_text();
        return kSuccessExitCode;
    }
    if (args.front() == "--config") {
        if (args.size() != 2U) {
            return report_invalid_arguments(
                "--config requires exactly one configuration path");
        }
        return run_with_config(args[1], false);
    }
    if (args.front() == "--serve") {
        if (args.size() != 3U || args[1] != "--config") {
            return report_invalid_arguments(
                "--serve requires --config followed by exactly one path");
        }
        return run_with_config(args[2], true);
    }
    return report_invalid_arguments(
        "unknown or conflicting command-line arguments");
}

std::string Application::version_text() {
    return std::string{"IndustrialAIServiceFramework "} + IAISF_VERSION_STRING;
}

std::string Application::usage_text() {
    return "Usage:\n"
           "  iaisf_server --help\n"
           "  iaisf_server --version\n"
           "  iaisf_server --config <path>\n"
           "  iaisf_server --serve --config <path>\n";
}

int Application::run_with_config(const std::string &path, const bool serve) {
    auto config_result = load_app_config(std::filesystem::path{path});
    if (!config_result) {
        report_failure(error_output_, "configuration", config_result.error());
        return kConfigurationExitCode;
    }
    const AppConfig &config = config_result.value();
    const LogLevel bootstrap_threshold =
        should_log(LogLevel::Info, config.logging.level) ? config.logging.level
                                                         : LogLevel::Info;
    ConsoleLogger logger{bootstrap_threshold, output_};

#if defined(IAISF_HAS_SERVICE_RUNTIME)
    auto runtime_result = service::make_runtime_options(config);
    if (!runtime_result) {
        report_failure(error_output_, "configuration", runtime_result.error());
        return kConfigurationExitCode;
    }
#endif

    logger.log(LogLevel::Info, "Application",
               "configuration validated for service " + config.service.name);
    logger.set_threshold(config.logging.level);
    if (!serve) {
        return kSuccessExitCode;
    }

#if !defined(IAISF_HAS_SERVICE_RUNTIME)
    error_output_ << "service startup failed [invalid_state]: "
                  << "serve mode requires the Linux service runtime\n";
    return kConfigurationExitCode;
#else
    service::RuntimeOptions runtime = std::move(runtime_result).value();
    auto loop_result = net::EventLoop::create(
        logger, runtime.reactor_max_events(),
        runtime.pending_callback_capacity(), runtime.timer_options());
    if (!loop_result) {
        report_failure(error_output_, "service startup", loop_result.error());
        return kConfigurationExitCode;
    }
    auto loop = std::move(loop_result).value();
    auto service_result = service::IndustrialAiService::create(
        *loop, logger, runtime.bind_endpoint(), runtime.service_options());
    if (!service_result) {
        report_failure(error_output_, "service startup",
                       service_result.error());
        return kConfigurationExitCode;
    }
    auto industrial_service = std::move(service_result).value();
    auto started = industrial_service->start();
    if (!started) {
        report_failure(error_output_, "service startup", started.error());
        return kConfigurationExitCode;
    }
    auto endpoint_text = industrial_service->local_endpoint().to_string();
    if (endpoint_text) {
        logger.log(LogLevel::Info, "Application",
                   "service started on " + endpoint_text.value());
    } else {
        logger.log(LogLevel::Info, "Application", "service started");
    }
    output_.flush();

    auto run_result = loop->run();
    if (!industrial_service->stopped()) {
        auto stopped = industrial_service->stop();
        if (!stopped) {
            report_failure(error_output_, "service shutdown", stopped.error());
            return kConfigurationExitCode;
        }
    }
    if (!run_result) {
        report_failure(error_output_, "service event loop", run_result.error());
        return kConfigurationExitCode;
    }
    if (!industrial_service->stopped()) {
        error_output_ << "service shutdown failed [internal_error]: "
                      << "service did not reach the stopped state\n";
        return kConfigurationExitCode;
    }
    return kSuccessExitCode;
#endif
}

int Application::report_invalid_arguments(const std::string &message) {
    error_output_ << "argument error: " << message << '\n' << usage_text();
    return kUsageExitCode;
}

} // namespace iaisf
