#include "iaisf/app/application.hpp"

#include <filesystem>
#include <string>
#include <string_view>

#include "iaisf/config/app_config.hpp"
#include "iaisf/core/error.hpp"
#include "iaisf/logging/console_logger.hpp"
#include "iaisf/version.hpp"

namespace iaisf {
namespace {

constexpr int kSuccessExitCode = 0;
constexpr int kConfigurationExitCode = 1;
constexpr int kUsageExitCode = 2;

}  // namespace

Application::Application(std::ostream& output, std::ostream& error_output)
    : output_(output), error_output_(error_output) {}

int Application::run(const std::vector<std::string>& args) {
    if (args.empty()) {
        output_ << version_text() << '\n'
                << "Phase 1 foundation is active; no network service is started.\n";
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
        return run_with_config(args[1]);
    }

    return report_invalid_arguments("unknown or conflicting command-line arguments");
}

std::string Application::version_text() {
    return std::string{"IndustrialAIServiceFramework "} + IAISF_VERSION_STRING;
}

std::string Application::usage_text() {
    return "Usage:\n"
           "  iaisf_server --help\n"
           "  iaisf_server --version\n"
           "  iaisf_server --config <path>\n";
}

int Application::run_with_config(const std::string& path) {
    auto config_result = load_app_config(std::filesystem::path{path});
    if (!config_result) {
        const Error& error = config_result.error();
        error_output_ << "configuration failed [" << to_string(error.code)
                      << "]: " << error.message << '\n';
        return kConfigurationExitCode;
    }

    const AppConfig& config = config_result.value();
    const LogLevel bootstrap_threshold =
        should_log(LogLevel::Info, config.log_level) ? config.log_level : LogLevel::Info;
    ConsoleLogger logger{bootstrap_threshold, output_};
    logger.log(
        LogLevel::Info,
        "Application",
        "configuration validated for service " + config.service_name);
    logger.set_threshold(config.log_level);
    return kSuccessExitCode;
}

int Application::report_invalid_arguments(const std::string& message) {
    error_output_ << "argument error: " << message << '\n' << usage_text();
    return kUsageExitCode;
}

}  // namespace iaisf
