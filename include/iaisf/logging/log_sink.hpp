#pragma once

#include <string_view>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/log_record.hpp"

namespace iaisf {

/** A destination written only by the AsyncLogger writer thread. */
class ILogSink {
public:
    virtual ~ILogSink() = default;

    /** Write one already formatted record. Implementations may throw. */
    virtual Result<void> write(
        const LogRecord& record,
        std::string_view formatted_record) = 0;

    /** Flush buffered destination data. Implementations may throw. */
    virtual Result<void> flush() = 0;
};

}  // namespace iaisf
