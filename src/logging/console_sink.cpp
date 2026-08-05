#include "iaisf/logging/console_sink.hpp"

#include <ostream>

namespace iaisf {

ConsoleSink::ConsoleSink(std::ostream& output) : output_(output) {}

Result<void> ConsoleSink::write(
    const LogRecord& /*record*/,
    const std::string_view formatted_record) {
    try {
        output_ << formatted_record;
        if (!output_) {
            return Result<void>::failure(
                make_error(ErrorCode::IoError, "console sink write failed"));
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(
            make_error(ErrorCode::IoError, "console sink write threw"));
    }
}

Result<void> ConsoleSink::flush() {
    try {
        output_.flush();
        if (!output_) {
            return Result<void>::failure(
                make_error(ErrorCode::IoError, "console sink flush failed"));
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(
            make_error(ErrorCode::IoError, "console sink flush threw"));
    }
}

}  // namespace iaisf
