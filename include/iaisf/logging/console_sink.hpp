#pragma once

#include <ostream>

#include "iaisf/logging/log_sink.hpp"

namespace iaisf {

/** Console sink used by the core logger and tests; the stream is borrowed. */
class ConsoleSink final : public ILogSink {
public:
    explicit ConsoleSink(std::ostream& output);

    Result<void> write(
        const LogRecord& record,
        std::string_view formatted_record) override;
    Result<void> flush() override;

private:
    std::ostream& output_;
};

}  // namespace iaisf
