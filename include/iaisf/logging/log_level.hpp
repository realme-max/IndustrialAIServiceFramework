#pragma once

#include <string_view>

#include "iaisf/core/result.hpp"

namespace iaisf {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;
[[nodiscard]] Result<LogLevel> parse_log_level(std::string_view text);
[[nodiscard]] bool should_log(LogLevel message_level, LogLevel threshold) noexcept;

}  // namespace iaisf

