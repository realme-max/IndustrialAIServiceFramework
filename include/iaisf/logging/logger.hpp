#pragma once

#include <string_view>

#include "iaisf/logging/log_level.hpp"

namespace iaisf {

class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void log(
        LogLevel level,
        std::string_view component,
        std::string_view message) = 0;
};

}  // namespace iaisf

