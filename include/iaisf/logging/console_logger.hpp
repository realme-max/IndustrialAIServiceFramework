#pragma once

#include <mutex>
#include <ostream>
#include <string_view>

#include "iaisf/logging/logger.hpp"

namespace iaisf {

/**
 * Synchronous, thread-safe console logger used by the Phase 1 foundation.
 *
 * The output stream is not owned and must outlive the logger. Callers must
 * not write to that stream concurrently outside this logger's mutex. This
 * class has no queue, file sink, rotation logic, or background thread.
 */
class ConsoleLogger final : public ILogger {
public:
    explicit ConsoleLogger(LogLevel threshold, std::ostream& output);

    void log(
        LogLevel level,
        std::string_view component,
        std::string_view message) override;

    void set_threshold(LogLevel threshold);
    [[nodiscard]] LogLevel threshold() const;

private:
    mutable std::mutex mutex_;
    LogLevel threshold_;
    std::ostream& output_;
};

}  // namespace iaisf
