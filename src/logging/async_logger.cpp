#include "iaisf/logging/async_logger.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <iomanip>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#include <signal.h>
#endif

namespace iaisf {
namespace {

constexpr std::uint64_t kFirstSequence = 1U;
constexpr std::string_view kWorkerFailure = "async logger worker failed";
constexpr std::string_view kSinkFailure = "async logger sink failed";

void increment_drop(AsyncLoggerStats& stats, const LogLevel level) noexcept {
    ++stats.dropped;
    switch (level) {
        case LogLevel::Trace:
            ++stats.dropped_trace;
            break;
        case LogLevel::Debug:
            ++stats.dropped_debug;
            break;
        case LogLevel::Info:
            ++stats.dropped_info;
            break;
        case LogLevel::Warn:
            ++stats.dropped_warn;
            break;
        case LogLevel::Error:
            ++stats.dropped_error;
            break;
    }
}

[[nodiscard]] bool is_utf8_continuation(const unsigned char value) noexcept {
    return (value & 0xC0U) == 0x80U;
}

/** Return one whole UTF-8 code point, or one byte for malformed input. */
[[nodiscard]] std::size_t utf8_codepoint_size(
    const std::string_view value,
    const std::size_t offset) noexcept {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::size_t expected = 1U;
    if (first >= 0xC2U && first <= 0xDFU) {
        expected = 2U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        expected = 3U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        expected = 4U;
    } else if (first >= 0x80U) {
        return 1U;
    }

    if (offset + expected > value.size()) {
        return 1U;
    }
    for (std::size_t index = 1U; index < expected; ++index) {
        if (!is_utf8_continuation(
                static_cast<unsigned char>(value[offset + index]))) {
            return 1U;
        }
    }

    // Reject overlong encodings and code points outside Unicode.  Invalid
    // bytes are retained as individual bytes rather than being dropped.
    if ((first == 0xE0U &&
         static_cast<unsigned char>(value[offset + 1U]) < 0xA0U) ||
        (first == 0xEDU &&
         static_cast<unsigned char>(value[offset + 1U]) > 0x9FU) ||
        (first == 0xF0U &&
         static_cast<unsigned char>(value[offset + 1U]) < 0x90U) ||
        (first == 0xF4U &&
         static_cast<unsigned char>(value[offset + 1U]) > 0x8FU)) {
        return 1U;
    }
    return expected;
}

[[nodiscard]] std::size_t utf8_prefix_bytes(
    const std::string_view value,
    const std::size_t limit) noexcept {
    const auto bounded_limit = std::min(value.size(), limit);
    std::size_t offset = 0U;
    while (offset < bounded_limit) {
        const auto codepoint_size = utf8_codepoint_size(value, offset);
        if (offset + codepoint_size > bounded_limit) {
            break;
        }
        offset += codepoint_size;
    }
    return offset;
}

std::string bounded_copy(const std::string_view value, const std::size_t limit) {
    const auto count = utf8_prefix_bytes(value, limit);
    return std::string{value.substr(0, count)};
}

void append_sanitized(
    std::string& output,
    const std::string_view value,
    const std::size_t limit) {
    const auto start = output.size();
    std::size_t appended = 0U;
    bool truncated = false;
    for (std::size_t index = 0U; index < value.size();) {
        const char raw_character = value[index];
        const auto character = static_cast<unsigned char>(raw_character);
        std::string_view replacement;
        char direct_character = '\0';
        switch (character) {
            case '\n':
                replacement = "\\n";
                break;
            case '\r':
                replacement = "\\r";
                break;
            case '\t':
                replacement = "\\t";
                break;
            default:
                if (character < 0x20U || character == 0x7FU) {
                    direct_character = '?';
                }
                break;
        }

        if (!replacement.empty()) {
            if (appended > limit || replacement.size() > limit - appended) {
                truncated = true;
                break;
            }
            output.append(replacement);
            appended += replacement.size();
            ++index;
        } else {
            const auto codepoint_size = utf8_codepoint_size(value, index);
            if (appended > limit || codepoint_size > limit - appended) {
                truncated = true;
                break;
            }
            if (direct_character != '\0') {
                output.push_back(direct_character);
            } else {
                output.append(value.substr(index, codepoint_size));
            }
            appended += direct_character == '\0' ? codepoint_size : 1U;
            index += codepoint_size;
        }
    }
    if (truncated && limit >= 3U) {
        const auto field = std::string_view{output}.substr(start);
        const auto prefix = utf8_prefix_bytes(field, limit - 3U);
        output.resize(start + prefix);
        output += "...";
    }
}

std::string utc_timestamp(const std::chrono::system_clock::time_point timestamp) {
    const auto since_epoch = timestamp.time_since_epoch();
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch) %
        std::chrono::seconds{1};
    const std::time_t current_time = std::chrono::system_clock::to_time_t(timestamp);

    std::tm utc_time{};
#if defined(_WIN32)
    gmtime_s(&utc_time, &current_time);
#else
    gmtime_r(&current_time, &utc_time);
#endif

    std::ostringstream timestamp_stream;
    timestamp_stream << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%S") << '.'
                     << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
    return timestamp_stream.str();
}

std::string format_record(const LogRecord& record, const std::size_t maximum_bytes) {
    std::ostringstream thread_stream;
    thread_stream << record.thread_id;

    std::string output;
    output.reserve(std::min(maximum_bytes, std::size_t{256}));
    output += utc_timestamp(record.timestamp);
    output += " [";
    output += to_string(record.level);
    output += "] [component=";
    append_sanitized(output, record.component, maximum_bytes);
    output += "] [thread=";
    append_sanitized(output, thread_stream.str(), maximum_bytes);
    output += "] [seq=";
    output += std::to_string(record.sequence);
    output += "] ";
    append_sanitized(output, record.message, maximum_bytes);
    output.push_back('\n');

    if (output.size() > maximum_bytes) {
        if (maximum_bytes == 0U) {
            return {};
        }
        output.resize(utf8_prefix_bytes(output, maximum_bytes - 1U));
        output.push_back('\n');
    }
    return output;
}

}  // namespace

struct AsyncLogger::Impl {
    explicit Impl(AsyncLoggerOptions logger_options,
                  std::vector<std::unique_ptr<ILogSink>> logger_sinks)
        : options(std::move(logger_options)), sinks(std::move(logger_sinks)) {}

    AsyncLoggerOptions options;
    std::vector<std::unique_ptr<ILogSink>> sinks;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<LogRecord> queue;
    std::thread writer;
    AsyncLoggerState logger_state{AsyncLoggerState::Running};
    bool join_in_progress{false};
    bool worker_drained{false};
    bool flush_requested{false};
    std::uint64_t flush_target{0};
    std::uint64_t flushed_sequence{0};
    std::uint64_t next_sequence{kFirstSequence};
    AsyncLoggerStats logger_stats;
    std::optional<Error> terminal_error;
    bool error_seen{false};
    std::chrono::steady_clock::time_point last_flush{
        std::chrono::steady_clock::now()};

    void remember_error(const ErrorCode code, const std::string_view message) noexcept {
        error_seen = true;
        try {
            if (!terminal_error.has_value()) {
                terminal_error = make_error(code, std::string{message});
            }
        } catch (...) {
            // A logger must not terminate the writer while reporting an error.
        }
    }
};

Result<AsyncLoggerOptions> AsyncLoggerOptions::create(
    const std::size_t queue_capacity,
    const std::size_t reserved_critical_capacity,
    const std::size_t batch_size,
    const std::size_t max_component_bytes,
    const std::size_t max_message_bytes,
    const std::size_t max_formatted_record_bytes,
    const LogLevel threshold) {
    if (queue_capacity == 0U || reserved_critical_capacity > queue_capacity ||
        batch_size == 0U || batch_size > queue_capacity || max_component_bytes == 0U ||
        max_message_bytes == 0U || max_formatted_record_bytes == 0U) {
        return Result<AsyncLoggerOptions>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid async logger options"));
    }

    AsyncLoggerOptions options;
    options.queue_capacity = queue_capacity;
    options.reserved_critical_capacity = reserved_critical_capacity;
    options.batch_size = batch_size;
    options.max_component_bytes = max_component_bytes;
    options.max_message_bytes = max_message_bytes;
    options.max_formatted_record_bytes = max_formatted_record_bytes;
    options.threshold = threshold;
    return Result<AsyncLoggerOptions>::success(std::move(options));
}

AsyncLogger::AsyncLogger(
    AsyncLoggerOptions options,
    std::vector<std::unique_ptr<ILogSink>> sinks)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(sinks))) {
    impl_->writer = std::thread([this]() {
        auto& state = *impl_;
#if defined(__linux__)
        // The application may create the logger before EventLoop installs its
        // signalfd mask. Keep shutdown signals out of this worker; the owner
        // thread is the sole delivery path for SIGINT/SIGTERM.
        sigset_t shutdown_signals{};
        if (::sigemptyset(&shutdown_signals) == 0 &&
            ::sigaddset(&shutdown_signals, SIGINT) == 0 &&
            ::sigaddset(&shutdown_signals, SIGTERM) == 0) {
            (void)::pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);
        }
#endif
        try {
            for (;;) {
                std::deque<LogRecord> batch;
                bool should_flush = false;
                std::uint64_t batch_last_sequence = 0U;
                {
                    std::unique_lock<std::mutex> lock{state.mutex};
                    bool timed_flush = false;
                    const auto ready = [&state]() {
                        return !state.queue.empty() || state.flush_requested ||
                               state.logger_state == AsyncLoggerState::Draining;
                    };
                    if (state.options.flush_interval_ms == 0U) {
                        state.condition.wait(lock, ready);
                    } else if (state.queue.empty() && !state.flush_requested &&
                               state.logger_state == AsyncLoggerState::Running) {
                        timed_flush = !state.condition.wait_for(
                            lock,
                            std::chrono::milliseconds{
                                state.options.flush_interval_ms},
                            ready);
                    } else {
                        state.condition.wait(lock, ready);
                    }

                    const auto count = std::min(state.options.batch_size, state.queue.size());
                    for (std::size_t index = 0U; index < count; ++index) {
                        batch.push_back(std::move(state.queue.front()));
                        state.queue.pop_front();
                    }
                    if (!batch.empty()) {
                        batch_last_sequence = batch.back().sequence;
                    }
                    const auto interval_elapsed =
                        std::chrono::steady_clock::now() - state.last_flush >=
                        std::chrono::milliseconds{state.options.flush_interval_ms};
                    should_flush = state.flush_requested || timed_flush ||
                                   state.logger_state == AsyncLoggerState::Draining ||
                                   (state.options.flush_interval_ms == 0U &&
                                    !batch.empty()) ||
                                   (state.options.flush_interval_ms != 0U &&
                                    !batch.empty() && interval_elapsed);
                }

                for (const auto& record : batch) {
                    try {
                        const auto formatted =
                            format_record(record, state.options.max_formatted_record_bytes);
                        for (const auto& sink : state.sinks) {
                            try {
                                const auto result = sink->write(record, formatted);
                                if (!result) {
                                    std::lock_guard<std::mutex> lock{state.mutex};
                                    ++state.logger_stats.sink_failures;
                                    state.remember_error(result.error().code, kSinkFailure);
                                }
                            } catch (...) {
                                std::lock_guard<std::mutex> lock{state.mutex};
                                ++state.logger_stats.sink_failures;
                                state.remember_error(ErrorCode::InternalError, kSinkFailure);
                            }
                        }
                    } catch (...) {
                        std::lock_guard<std::mutex> lock{state.mutex};
                        ++state.logger_stats.sink_failures;
                        state.remember_error(ErrorCode::InternalError, kWorkerFailure);
                    }
                }

                if (should_flush) {
                    for (const auto& sink : state.sinks) {
                        try {
                            const auto result = sink->flush();
                            if (!result) {
                                std::lock_guard<std::mutex> lock{state.mutex};
                                ++state.logger_stats.sink_failures;
                                state.remember_error(result.error().code, kSinkFailure);
                            }
                        } catch (...) {
                            std::lock_guard<std::mutex> lock{state.mutex};
                            ++state.logger_stats.sink_failures;
                            state.remember_error(ErrorCode::InternalError, kSinkFailure);
                        }
                    }
                    state.last_flush = std::chrono::steady_clock::now();
                }

                bool finish_draining = false;
                {
                    std::lock_guard<std::mutex> lock{state.mutex};
                    if (batch_last_sequence > state.flushed_sequence) {
                        state.flushed_sequence = batch_last_sequence;
                    }
                    if (state.flush_requested &&
                        state.flushed_sequence >= state.flush_target) {
                        state.flush_requested = false;
                    }
                    finish_draining =
                        state.logger_state == AsyncLoggerState::Draining &&
                        state.queue.empty() && !state.flush_requested;
                    state.condition.notify_all();
                }

                if (finish_draining) {
                    // Sink callbacks are user code and must never run while the
                    // logger mutex is held (a sink may call flush/shutdown).
                    for (const auto& sink : state.sinks) {
                        try {
                            const auto result = sink->flush();
                            if (!result) {
                                std::lock_guard<std::mutex> lock{state.mutex};
                                ++state.logger_stats.sink_failures;
                                state.remember_error(result.error().code, kSinkFailure);
                            }
                        } catch (...) {
                            std::lock_guard<std::mutex> lock{state.mutex};
                            ++state.logger_stats.sink_failures;
                            state.remember_error(ErrorCode::InternalError, kSinkFailure);
                        }
                    }
                    std::lock_guard<std::mutex> lock{state.mutex};
                    state.worker_drained = true;
                    state.condition.notify_all();
                    return;
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock{state.mutex};
            state.remember_error(ErrorCode::InternalError, kWorkerFailure);
            state.queue.clear();
            state.flush_requested = false;
            state.logger_state = AsyncLoggerState::Draining;
            state.worker_drained = true;
            state.condition.notify_all();
        }
    });
}

Result<std::unique_ptr<AsyncLogger>> AsyncLogger::create(
    AsyncLoggerOptions options,
    std::vector<std::unique_ptr<ILogSink>> sinks) {
    if (options.queue_capacity == 0U || options.reserved_critical_capacity > options.queue_capacity ||
        options.batch_size == 0U || options.batch_size > options.queue_capacity ||
        options.max_component_bytes == 0U || options.max_message_bytes == 0U ||
        options.max_formatted_record_bytes == 0U ||
        options.flush_interval_ms > 60ULL * 60ULL * 1000ULL || sinks.empty() ||
        std::any_of(sinks.begin(), sinks.end(), [](const auto& sink) { return sink == nullptr; })) {
        return Result<std::unique_ptr<AsyncLogger>>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid async logger configuration"));
    }
    try {
        auto logger = std::unique_ptr<AsyncLogger>{
            new AsyncLogger{std::move(options), std::move(sinks)}};
        return Result<std::unique_ptr<AsyncLogger>>::success(std::move(logger));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<AsyncLogger>>::failure(
            make_error(ErrorCode::ResourceExhausted, "async logger allocation failed"));
    } catch (...) {
        return Result<std::unique_ptr<AsyncLogger>>::failure(
            make_error(ErrorCode::SystemError, "async logger initialization failed"));
    }
}

AsyncLogger::~AsyncLogger() noexcept {
    try {
        (void)shutdown();
    } catch (...) {
        // Destruction must not throw; shutdown still joins the writer on normal paths.
    }
}

void AsyncLogger::log(
    const LogLevel level,
    const std::string_view component,
    const std::string_view message) noexcept {
    auto& state = *impl_;
    try {
        LogRecord record;
        record.timestamp = std::chrono::system_clock::now();
        record.level = level;
        record.component = bounded_copy(component, state.options.max_component_bytes);
        record.message = bounded_copy(message, state.options.max_message_bytes);
        record.thread_id = std::this_thread::get_id();

        std::lock_guard<std::mutex> lock{state.mutex};
        if (state.logger_state != AsyncLoggerState::Running || state.worker_drained) {
            ++state.logger_stats.rejected_after_shutdown;
            increment_drop(state.logger_stats, level);
            return;
        }
        if (!should_log(level, state.options.threshold)) {
            ++state.logger_stats.filtered;
            return;
        }

        const auto normal_limit =
            state.options.queue_capacity - state.options.reserved_critical_capacity;
        const bool critical = level == LogLevel::Warn || level == LogLevel::Error;
        if (state.queue.size() >= state.options.queue_capacity ||
            (!critical && state.queue.size() >= normal_limit)) {
            increment_drop(state.logger_stats, level);
            return;
        }

        record.sequence = state.next_sequence;
        ++state.next_sequence;
        state.queue.push_back(std::move(record));
        ++state.logger_stats.accepted;
        state.condition.notify_one();
    } catch (...) {
        std::lock_guard<std::mutex> lock{state.mutex};
        increment_drop(state.logger_stats, level);
    }
}

Result<void> AsyncLogger::flush() {
    auto& state = *impl_;
    std::unique_lock<std::mutex> lock{state.mutex};
    if (state.writer.joinable() && state.writer.get_id() == std::this_thread::get_id()) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidState, "logger writer cannot flush itself"));
    }
    if (state.logger_state == AsyncLoggerState::Stopped || state.worker_drained) {
        if (state.terminal_error.has_value()) {
            return Result<void>::failure(*state.terminal_error);
        }
        if (state.error_seen) {
            return Result<void>::failure(
                make_error(ErrorCode::InternalError, "async logger failed"));
        }
        return Result<void>::success();
    }
    const auto target = state.next_sequence == kFirstSequence ? 0U : state.next_sequence - 1U;
    state.flush_target = std::max(state.flush_target, target);
    state.flush_requested = true;
    state.condition.notify_one();
    state.condition.wait(lock, [&state]() {
        return (!state.flush_requested && state.flushed_sequence >= state.flush_target) ||
               state.logger_state == AsyncLoggerState::Stopped || state.worker_drained;
    });
    if (state.terminal_error.has_value()) {
        return Result<void>::failure(*state.terminal_error);
    }
    if (state.error_seen) {
        return Result<void>::failure(
            make_error(ErrorCode::InternalError, "async logger failed"));
    }
    return Result<void>::success();
}

Result<void> AsyncLogger::shutdown() {
    auto& state = *impl_;
    std::unique_lock<std::mutex> lock{state.mutex};
    if (state.writer.joinable() && state.writer.get_id() == std::this_thread::get_id()) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidState, "logger writer cannot join itself"));
    }
    if (state.logger_state == AsyncLoggerState::Stopped) {
        if (state.terminal_error.has_value()) {
            return Result<void>::failure(*state.terminal_error);
        }
        if (state.error_seen) {
            return Result<void>::failure(
                make_error(ErrorCode::InternalError, "async logger failed"));
        }
        return Result<void>::success();
    }
    if (state.logger_state == AsyncLoggerState::Running) {
        state.logger_state = AsyncLoggerState::Draining;
    }
    if (state.join_in_progress) {
        state.condition.wait(lock, [&state]() {
            return state.logger_state == AsyncLoggerState::Stopped;
        });
        if (state.terminal_error.has_value()) {
            return Result<void>::failure(*state.terminal_error);
        }
        if (state.error_seen) {
            return Result<void>::failure(
                make_error(ErrorCode::InternalError, "async logger failed"));
        }
        return Result<void>::success();
    }
    state.join_in_progress = true;
    state.condition.notify_one();
    state.condition.wait(lock, [&state]() { return state.worker_drained; });
    lock.unlock();

    try {
        state.writer.join();
    } catch (...) {
        std::lock_guard<std::mutex> failure_lock{state.mutex};
        state.join_in_progress = false;
        state.logger_state = AsyncLoggerState::Stopped;
        state.remember_error(ErrorCode::SystemError, "logger writer join failed");
        state.condition.notify_all();
        if (state.terminal_error.has_value()) {
            return Result<void>::failure(*state.terminal_error);
        }
        return Result<void>::failure(
            make_error(ErrorCode::SystemError, "logger writer join failed"));
    }

    lock.lock();
    state.logger_state = AsyncLoggerState::Stopped;
    state.join_in_progress = false;
    state.condition.notify_all();
    if (state.terminal_error.has_value()) {
        return Result<void>::failure(*state.terminal_error);
    }
    if (state.error_seen) {
        return Result<void>::failure(
            make_error(ErrorCode::InternalError, "async logger failed"));
    }
    return Result<void>::success();
}

void AsyncLogger::set_threshold(const LogLevel threshold) noexcept {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    impl_->options.threshold = threshold;
}

LogLevel AsyncLogger::threshold() const noexcept {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return impl_->options.threshold;
}

AsyncLoggerState AsyncLogger::state() const noexcept {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return impl_->logger_state;
}

AsyncLoggerStats AsyncLogger::stats() const noexcept {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return impl_->logger_stats;
}

}  // namespace iaisf
