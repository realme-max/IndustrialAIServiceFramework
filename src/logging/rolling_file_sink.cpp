#include "iaisf/logging/rolling_file_sink.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

namespace iaisf {
namespace {

constexpr std::size_t kMaximumArchives = 1000U;

[[nodiscard]] Error file_error(const std::string_view message) {
    return make_error(ErrorCode::IoError, std::string{message});
}

[[nodiscard]] std::filesystem::path archive_path(
    const std::filesystem::path& current,
    const std::size_t index) {
    auto archive = current;
    archive += ".";
    archive += std::to_string(index);
    return archive;
}

}  // namespace

struct RollingFileSink::Impl {
    explicit Impl(RollingFileSinkOptions sink_options)
        : options(std::move(sink_options)) {}

    RollingFileSinkOptions options;
    std::ofstream file;
    std::uintmax_t current_size{0U};
};

Result<RollingFileSinkOptions> RollingFileSinkOptions::create(
    std::filesystem::path file_path,
    const std::uintmax_t max_file_bytes,
    const std::size_t max_archives) {
    if (file_path.empty() || max_file_bytes == 0U || max_archives > kMaximumArchives) {
        return Result<RollingFileSinkOptions>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid rolling file sink options"));
    }

    RollingFileSinkOptions options;
    options.file_path = std::move(file_path);
    options.max_file_bytes = max_file_bytes;
    options.max_archives = max_archives;
    return Result<RollingFileSinkOptions>::success(std::move(options));
}

RollingFileSink::RollingFileSink(RollingFileSinkOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

RollingFileSink::~RollingFileSink() noexcept {
    if (impl_ != nullptr && impl_->file.is_open()) {
        impl_->file.close();
    }
}

Result<std::unique_ptr<RollingFileSink>> RollingFileSink::create(
    RollingFileSinkOptions options) {
    if (options.file_path.empty() || options.max_file_bytes == 0U ||
        options.max_archives > kMaximumArchives) {
        return Result<std::unique_ptr<RollingFileSink>>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid rolling file sink options"));
    }

    try {
        auto sink = std::unique_ptr<RollingFileSink>{
            new RollingFileSink{std::move(options)}};
        const auto opened = sink->open_append();
        if (!opened) {
            return Result<std::unique_ptr<RollingFileSink>>::failure(opened.error());
        }
        return Result<std::unique_ptr<RollingFileSink>>::success(std::move(sink));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<RollingFileSink>>::failure(
            make_error(ErrorCode::ResourceExhausted, "rolling file sink allocation failed"));
    } catch (...) {
        return Result<std::unique_ptr<RollingFileSink>>::failure(
            file_error("rolling file sink initialization failed"));
    }
}

Result<void> RollingFileSink::open_append() {
    auto& state = *impl_;
    state.file.clear();
    state.file.rdbuf()->pubsetbuf(nullptr, 0);
    state.file.open(
        state.options.file_path,
        std::ios::out | std::ios::app | std::ios::binary);
    if (!state.file.is_open() || !state.file) {
        state.file.close();
        return Result<void>::failure(file_error("rolling file sink open failed"));
    }

    std::error_code error;
    const auto size = std::filesystem::file_size(state.options.file_path, error);
    if (error != std::error_code{}) {
        // Some platforms expose special writable devices without a portable
        // file_size value. The stream remains authoritative for the first
        // write; regular files report their exact size above.
        state.current_size = 0U;
    } else {
        state.current_size = size;
    }
    return Result<void>::success();
}

Result<void> RollingFileSink::open_truncated() {
    auto& state = *impl_;
    state.file.clear();
    state.file.rdbuf()->pubsetbuf(nullptr, 0);
    state.file.open(
        state.options.file_path,
        std::ios::out | std::ios::trunc | std::ios::binary);
    if (!state.file.is_open() || !state.file) {
        state.file.close();
        return Result<void>::failure(file_error("rolling file sink reopen failed"));
    }
    state.current_size = 0U;
    return Result<void>::success();
}

Result<void> RollingFileSink::reopen_after_rotation_failure(const Error& error) {
    const auto reopened = open_append();
    (void)reopened;
    return Result<void>::failure(error);
}

Result<void> RollingFileSink::rotate() {
    auto& state = *impl_;
    state.file.flush();
    if (!state.file) {
        state.file.close();
        return Result<void>::failure(file_error("rolling file sink flush before rotate failed"));
    }
    state.file.close();

    if (state.options.max_archives == 0U) {
        const auto truncated = open_truncated();
        if (!truncated) {
            return Result<void>::failure(truncated.error());
        }
        return Result<void>::success();
    }

    std::error_code error;
    const auto oldest = archive_path(state.options.file_path, state.options.max_archives);
    std::filesystem::remove(oldest, error);
    if (error != std::error_code{}) {
        return reopen_after_rotation_failure(
            file_error("rolling file sink remove oldest archive failed"));
    }

    for (std::size_t index = state.options.max_archives; index > 1U; --index) {
        const auto source = archive_path(state.options.file_path, index - 1U);
        const auto destination = archive_path(state.options.file_path, index);
        error.clear();
        if (!std::filesystem::exists(source, error)) {
            if (error != std::error_code{}) {
                return reopen_after_rotation_failure(
                    file_error("rolling file sink archive lookup failed"));
            }
            continue;
        }
        std::filesystem::rename(source, destination, error);
        if (error != std::error_code{}) {
            return reopen_after_rotation_failure(
                file_error("rolling file sink archive rename failed"));
        }
    }

    error.clear();
    std::filesystem::rename(
        state.options.file_path,
        archive_path(state.options.file_path, 1U),
        error);
    if (error != std::error_code{}) {
        return reopen_after_rotation_failure(
            file_error("rolling file sink current file rename failed"));
    }

    const auto opened = open_append();
    if (!opened) {
        return Result<void>::failure(opened.error());
    }
    return Result<void>::success();
}

Result<void> RollingFileSink::write(
    const LogRecord& /*record*/,
    const std::string_view formatted_record) {
    auto& state = *impl_;
    if (!state.file.is_open() || !state.file) {
        return Result<void>::failure(file_error("rolling file sink is not writable"));
    }

    const auto record_size = static_cast<std::uintmax_t>(formatted_record.size());
    const bool exceeds_limit = state.current_size > state.options.max_file_bytes ||
                               record_size > state.options.max_file_bytes - state.current_size;
    if (exceeds_limit && state.current_size != 0U) {
        const auto rotated = rotate();
        if (!rotated) {
            return rotated;
        }
    }

    const auto maximum_stream_size =
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max());
    if (formatted_record.size() > maximum_stream_size) {
        return Result<void>::failure(file_error("rolling file sink record is too large"));
    }
    state.file.write(formatted_record.data(), static_cast<std::streamsize>(formatted_record.size()));
    if (!state.file) {
        return Result<void>::failure(file_error("rolling file sink write failed"));
    }
    state.current_size += record_size;
    return Result<void>::success();
}

Result<void> RollingFileSink::flush() {
    auto& state = *impl_;
    if (!state.file.is_open() || !state.file) {
        return Result<void>::failure(file_error("rolling file sink is not open"));
    }
    state.file.flush();
    if (!state.file) {
        return Result<void>::failure(file_error("rolling file sink flush failed"));
    }
    return Result<void>::success();
}

}  // namespace iaisf
