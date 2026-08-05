#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/logging/log_sink.hpp"

namespace iaisf {

enum class AsyncLoggerState {
    Running,
    Draining,
    Stopped,
};

struct AsyncLoggerOptions {
    /** Maximum number of queued records, including reserved critical slots. */
    std::size_t queue_capacity{1024};
    /** Warn/Error records may use these slots after normal capacity is full. */
    std::size_t reserved_critical_capacity{32};
    std::size_t batch_size{64};
    std::size_t max_component_bytes{128};
    std::size_t max_message_bytes{4096};
    std::size_t max_formatted_record_bytes{8192};
    /** Maximum interval between sink flushes while records are flowing. */
    std::uint64_t flush_interval_ms{1000U};
    LogLevel threshold{LogLevel::Info};

    [[nodiscard]] static Result<AsyncLoggerOptions> create(
        std::size_t queue_capacity = 1024,
        std::size_t reserved_critical_capacity = 32,
        std::size_t batch_size = 64,
        std::size_t max_component_bytes = 128,
        std::size_t max_message_bytes = 4096,
        std::size_t max_formatted_record_bytes = 8192,
        LogLevel threshold = LogLevel::Info);
};

struct AsyncLoggerStats {
    std::uint64_t accepted{0};
    std::uint64_t filtered{0};
    /** Records rejected because the bounded queue or logger lifecycle could not accept them. */
    std::uint64_t dropped{0};
    std::uint64_t dropped_trace{0};
    std::uint64_t dropped_debug{0};
    std::uint64_t dropped_info{0};
    std::uint64_t dropped_warn{0};
    std::uint64_t dropped_error{0};
    std::uint64_t rejected_after_shutdown{0};
    std::uint64_t sink_failures{0};
};

/**
 * A bounded, multi-producer/single-writer logger.
 *
 * Producers only hold the queue mutex briefly and never wait for free queue
 * space. One owned writer thread drains the queue and is the only caller of
 * sinks. AsyncLogger must outlive all producer calls and its sinks.
 */
class AsyncLogger final : public ILogger {
public:
    [[nodiscard]] static Result<std::unique_ptr<AsyncLogger>> create(
        AsyncLoggerOptions options,
        std::vector<std::unique_ptr<ILogSink>> sinks);

    ~AsyncLogger() noexcept override;

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    AsyncLogger& operator=(AsyncLogger&&) = delete;

    void log(
        LogLevel level,
        std::string_view component,
        std::string_view message) noexcept override;

    /** Wait until all records accepted before this call reach the sinks. */
    [[nodiscard]] Result<void> flush();

    /** Transition to draining, join the writer, and become stopped. */
    [[nodiscard]] Result<void> shutdown();

    void set_threshold(LogLevel threshold) noexcept;
    [[nodiscard]] LogLevel threshold() const noexcept;
    [[nodiscard]] AsyncLoggerState state() const noexcept;
    [[nodiscard]] AsyncLoggerStats stats() const noexcept;

private:
    AsyncLogger(AsyncLoggerOptions options, std::vector<std::unique_ptr<ILogSink>> sinks);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace iaisf
