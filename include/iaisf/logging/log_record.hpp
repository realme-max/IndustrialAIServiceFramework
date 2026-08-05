#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "iaisf/logging/log_level.hpp"

namespace iaisf {

/** A single immutable-at-dispatch log event. */
struct LogRecord {
    std::chrono::system_clock::time_point timestamp{};
    LogLevel level{LogLevel::Info};
    std::string component;
    std::string message;
    std::thread::id thread_id{};
    std::uint64_t sequence{0};
};

}  // namespace iaisf
