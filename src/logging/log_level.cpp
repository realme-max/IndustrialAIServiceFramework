#include "iaisf/logging/log_level.hpp"

#include <string>

namespace iaisf {
namespace {

int severity_rank(const LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return 0;
        case LogLevel::Debug:
            return 1;
        case LogLevel::Info:
            return 2;
        case LogLevel::Warn:
            return 3;
        case LogLevel::Error:
            return 4;
    }
    return -1;
}

}  // namespace

std::string_view to_string(const LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
    }
    return "UNKNOWN";
}

Result<LogLevel> parse_log_level(const std::string_view text) {
    if (text == "trace") {
        return Result<LogLevel>::success(LogLevel::Trace);
    }
    if (text == "debug") {
        return Result<LogLevel>::success(LogLevel::Debug);
    }
    if (text == "info") {
        return Result<LogLevel>::success(LogLevel::Info);
    }
    if (text == "warn") {
        return Result<LogLevel>::success(LogLevel::Warn);
    }
    if (text == "error") {
        return Result<LogLevel>::success(LogLevel::Error);
    }
    return Result<LogLevel>::failure(
        make_error(ErrorCode::InvalidArgument, "unsupported log level"));
}

bool should_log(const LogLevel message_level, const LogLevel threshold) noexcept {
    const int message_rank = severity_rank(message_level);
    const int threshold_rank = severity_rank(threshold);
    return message_rank >= 0 && threshold_rank >= 0 && message_rank >= threshold_rank;
}

}  // namespace iaisf

