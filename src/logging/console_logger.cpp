#include "iaisf/logging/console_logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace iaisf {
namespace {

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch) %
        std::chrono::seconds{1};
    const std::time_t current_time = std::chrono::system_clock::to_time_t(now);

    std::tm utc_time{};
#if defined(_WIN32)
    gmtime_s(&utc_time, &current_time);
#else
    gmtime_r(&current_time, &utc_time);
#endif

    std::ostringstream timestamp;
    timestamp << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%S") << '.'
              << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
    return timestamp.str();
}

std::string sanitize_log_field(const std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char raw_character : input) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (character < 0x20U || character == 0x7FU) {
                    output += '?';
                } else {
                    output.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    return output;
}

}  // namespace

ConsoleLogger::ConsoleLogger(const LogLevel threshold, std::ostream& output)
    : threshold_(threshold), output_(output) {}

void ConsoleLogger::log(
    const LogLevel level,
    const std::string_view component,
    const std::string_view message) {
    std::lock_guard<std::mutex> lock{mutex_};
    if (!should_log(level, threshold_)) {
        return;
    }

    output_ << utc_timestamp() << " [" << to_string(level) << "] ["
            << sanitize_log_field(component) << "] " << sanitize_log_field(message)
            << '\n';
}

void ConsoleLogger::set_threshold(const LogLevel threshold) {
    std::lock_guard<std::mutex> lock{mutex_};
    threshold_ = threshold;
}

LogLevel ConsoleLogger::threshold() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return threshold_;
}

}  // namespace iaisf
