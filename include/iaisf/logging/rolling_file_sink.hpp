#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/log_sink.hpp"

namespace iaisf {

struct RollingFileSinkOptions {
    std::filesystem::path file_path;
    std::uintmax_t max_file_bytes{10U * 1024U * 1024U};
    std::size_t max_archives{3U};

    [[nodiscard]] static Result<RollingFileSinkOptions> create(
        std::filesystem::path file_path,
        std::uintmax_t max_file_bytes = 10U * 1024U * 1024U,
        std::size_t max_archives = 3U);
};

/**
 * Append-only file sink with bounded numbered archives.
 *
 * The sink owns one file handle and is intended to be called by the
 * AsyncLogger writer thread only. It never splits one formatted record across
 * files: a record is written only after a successful rotation when needed.
 */
class RollingFileSink final : public ILogSink {
public:
    [[nodiscard]] static Result<std::unique_ptr<RollingFileSink>> create(
        RollingFileSinkOptions options);

    ~RollingFileSink() noexcept override;

    RollingFileSink(const RollingFileSink&) = delete;
    RollingFileSink& operator=(const RollingFileSink&) = delete;
    RollingFileSink(RollingFileSink&&) = delete;
    RollingFileSink& operator=(RollingFileSink&&) = delete;

    Result<void> write(
        const LogRecord& record,
        std::string_view formatted_record) override;
    Result<void> flush() override;

private:
    explicit RollingFileSink(RollingFileSinkOptions options);

    Result<void> open_append();
    Result<void> open_truncated();
    Result<void> rotate();
    Result<void> reopen_after_rotation_failure(const Error& error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iaisf
