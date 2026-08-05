#include "iaisf/logging/async_logger.hpp"
#include "iaisf/logging/console_sink.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace iaisf {
namespace {

class CapturingSink final : public ILogSink {
public:
    Result<void> write(
        const LogRecord& record,
        const std::string_view formatted_record) override {
        std::lock_guard<std::mutex> lock{mutex_};
        records_.push_back(record);
        formatted_.emplace_back(formatted_record);
        condition_.notify_all();
        return Result<void>::success();
    }

    Result<void> flush() override {
        std::lock_guard<std::mutex> lock{mutex_};
        ++flushes_;
        condition_.notify_all();
        return Result<void>::success();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock{mutex_};
        return records_.size();
    }

    [[nodiscard]] std::string formatted() const {
        std::lock_guard<std::mutex> lock{mutex_};
        std::string output;
        for (const auto& item : formatted_) {
            output += item;
        }
        return output;
    }

    [[nodiscard]] LogRecord record_at(const std::size_t index) const {
        std::lock_guard<std::mutex> lock{mutex_};
        return records_.at(index);
    }

    void wait_for_size(const std::size_t expected) {
        std::unique_lock<std::mutex> lock{mutex_};
        condition_.wait(lock, [this, expected]() { return records_.size() >= expected; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<LogRecord> records_;
    std::vector<std::string> formatted_;
    std::size_t flushes_{0U};
};

class BlockingSink final : public ILogSink {
public:
    Result<void> write(
        const LogRecord& /*record*/,
        const std::string_view /*formatted_record*/) override {
        std::unique_lock<std::mutex> lock{mutex_};
        if (!entered_) {
            entered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this]() { return released_; });
        }
        ++writes_;
        return Result<void>::success();
    }

    Result<void> flush() override {
        return Result<void>::success();
    }

    void wait_until_entered() {
        std::unique_lock<std::mutex> lock{mutex_};
        condition_.wait(lock, [this]() { return entered_; });
    }

    void release() {
        std::lock_guard<std::mutex> lock{mutex_};
        released_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool entered_{false};
    bool released_{false};
    std::size_t writes_{0U};
};

class ThrowingSink final : public ILogSink {
public:
    Result<void> write(
        const LogRecord& /*record*/,
        const std::string_view /*formatted_record*/) override {
        throw std::runtime_error{"sink failure detail must not escape"};
    }

    Result<void> flush() override {
        throw std::runtime_error{"sink flush failure"};
    }
};

class FlushFailingSink final : public ILogSink {
public:
    Result<void> write(
        const LogRecord& /*record*/,
        const std::string_view /*formatted_record*/) override {
        return Result<void>::success();
    }

    Result<void> flush() override {
        return Result<void>::failure(
            make_error(ErrorCode::IoError, "flush failure must remain observable"));
    }
};

class SelfShutdownSink final : public ILogSink {
public:
    void set_logger(AsyncLogger* logger) noexcept {
        logger_ = logger;
    }

    Result<void> write(
        const LogRecord& /*record*/,
        const std::string_view /*formatted_record*/) override {
        if (logger_ != nullptr) {
            self_result_ = logger_->shutdown();
        }
        return Result<void>::success();
    }

    Result<void> flush() override {
        return Result<void>::success();
    }

    [[nodiscard]] const Result<void>& self_result() const {
        return self_result_;
    }

private:
    AsyncLogger* logger_{nullptr};
    Result<void> self_result_{Result<void>::success()};
};

Result<std::unique_ptr<AsyncLogger>> make_logger(
    AsyncLoggerOptions options,
    std::unique_ptr<ILogSink> sink) {
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(std::move(sink));
    return AsyncLogger::create(std::move(options), std::move(sinks));
}

AsyncLoggerOptions options_for_tests() {
    auto options = AsyncLoggerOptions::create(128U, 0U, 8U, 128U, 1024U, 4096U, LogLevel::Trace);
    EXPECT_TRUE(options.has_value());
    return options.value();
}

TEST(AsyncLoggerTest, FiltersBelowThresholdAndFormatsControlCharacters) {
    auto options = options_for_tests();
    options.threshold = LogLevel::Warn;
    auto sink = std::make_unique<CapturingSink>();
    auto* sink_ptr = sink.get();
    auto logger_result = make_logger(std::move(options), std::move(sink));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());

    logger->log(LogLevel::Info, "component", "filtered");
    logger->log(LogLevel::Warn, "line\ncomponent", "message\twith\x01" "control");
    ASSERT_TRUE(logger->flush().has_value());

    EXPECT_EQ(sink_ptr->size(), 1U);
    const auto output = sink_ptr->formatted();
    EXPECT_NE(output.find("[WARN]"), std::string::npos);
    EXPECT_NE(output.find("line\\ncomponent"), std::string::npos);
    EXPECT_NE(output.find("message\\twith?control"), std::string::npos);
    EXPECT_NE(output.find("[thread="), std::string::npos);
    EXPECT_NE(output.find("[seq=1]"), std::string::npos);
    EXPECT_EQ(logger->stats().filtered, 1U);
    ASSERT_TRUE(logger->shutdown().has_value());
}

bool is_valid_utf8(const std::string_view value) {
    for (std::size_t index = 0U; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t width = 1U;
        if (first <= 0x7FU) {
            width = 1U;
        } else if (first >= 0xC2U && first <= 0xDFU) {
            width = 2U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            width = 3U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            width = 4U;
        } else {
            return false;
        }
        if (index + width > value.size()) {
            return false;
        }
        for (std::size_t continuation = 1U; continuation < width; ++continuation) {
            const auto byte = static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
        }
        index += width;
    }
    return true;
}

TEST(AsyncLoggerTest, PreservesUtf8BoundariesWithinFormatterLimits) {
    auto options = AsyncLoggerOptions::create(8U, 0U, 4U, 32U, 64U, 96U, LogLevel::Trace);
    ASSERT_TRUE(options.has_value());
    auto sink = std::make_unique<CapturingSink>();
    auto* sink_ptr = sink.get();
    auto logger_result = make_logger(options.value(), std::move(sink));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());

    logger->log(LogLevel::Info, "组件🙂组件🙂", "内容🙂内容🙂内容🙂\n\r\t");
    ASSERT_TRUE(logger->flush().has_value());

    const auto output = sink_ptr->formatted();
    ASSERT_LE(output.size(), 96U);
    ASSERT_FALSE(output.empty());
    EXPECT_EQ(output.back(), '\n');
    EXPECT_TRUE(is_valid_utf8(output));
    ASSERT_TRUE(logger->shutdown().has_value());
}

TEST(AsyncLoggerTest, BoundsOversizedComponentAndMessageByBytes) {
    auto options = AsyncLoggerOptions::create(8U, 0U, 4U, 5U, 7U, 512U, LogLevel::Trace);
    ASSERT_TRUE(options.has_value());
    auto sink = std::make_unique<CapturingSink>();
    auto* sink_ptr = sink.get();
    auto logger_result = make_logger(options.value(), std::move(sink));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());

    logger->log(LogLevel::Info, "🙂🙂🙂", "内容内容内容");
    ASSERT_TRUE(logger->flush().has_value());
    ASSERT_EQ(sink_ptr->size(), 1U);
    const auto captured = sink_ptr->record_at(0U);
    EXPECT_LE(captured.component.size(), 5U);
    EXPECT_LE(captured.message.size(), 7U);
    EXPECT_TRUE(is_valid_utf8(captured.component));
    EXPECT_TRUE(is_valid_utf8(captured.message));
    ASSERT_TRUE(logger->shutdown().has_value());
}

TEST(AsyncLoggerTest, QueueFullDropsNormalRecordsButReservesCriticalCapacity) {
    auto options = AsyncLoggerOptions::create(3U, 1U, 1U, 64U, 256U, 1024U, LogLevel::Trace);
    ASSERT_TRUE(options.has_value());
    auto sink = std::make_unique<BlockingSink>();
    auto* sink_ptr = sink.get();
    auto logger_result = make_logger(options.value(), std::move(sink));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());

    logger->log(LogLevel::Error, "test", "first");
    sink_ptr->wait_until_entered();
    logger->log(LogLevel::Info, "test", "queued-1");
    logger->log(LogLevel::Info, "test", "queued-2");
    logger->log(LogLevel::Info, "test", "dropped-normal");
    logger->log(LogLevel::Error, "test", "reserved-error");

    EXPECT_EQ(logger->stats().dropped_info, 1U);
    EXPECT_EQ(logger->stats().dropped, 1U);
    sink_ptr->release();
    ASSERT_TRUE(logger->shutdown().has_value());
}

TEST(AsyncLoggerTest, FlushWaitsForAcceptedRecords) {
    auto logger_result = make_logger(options_for_tests(), std::make_unique<CapturingSink>());
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    logger->log(LogLevel::Info, "flush", "record");
    EXPECT_TRUE(logger->flush().has_value());
    EXPECT_EQ(logger->stats().accepted, 1U);
    EXPECT_TRUE(logger->shutdown().has_value());
}

TEST(AsyncLoggerTest, ShutdownIsIdempotentAndRejectsNewRecords) {
    auto sink = std::make_unique<CapturingSink>();
    auto* sink_ptr = sink.get();
    auto logger_result = make_logger(options_for_tests(), std::move(sink));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    logger->log(LogLevel::Info, "shutdown", "before");
    ASSERT_TRUE(logger->shutdown().has_value());
    EXPECT_TRUE(logger->shutdown().has_value());
    logger->log(LogLevel::Error, "shutdown", "after");
    EXPECT_EQ(logger->state(), AsyncLoggerState::Stopped);
    EXPECT_EQ(sink_ptr->size(), 1U);
    EXPECT_EQ(logger->stats().rejected_after_shutdown, 1U);
    EXPECT_EQ(logger->stats().dropped, 1U);
}

TEST(AsyncLoggerTest, ProducersRacingShutdownAreAccountedExactlyOnce) {
    auto options = AsyncLoggerOptions::create(4096U, 0U, 32U, 64U, 256U, 1024U, LogLevel::Trace);
    ASSERT_TRUE(options.has_value());
    auto logger_result = make_logger(options.value(), std::make_unique<CapturingSink>());
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());

    constexpr std::size_t producer_count = 6U;
    constexpr std::size_t records_per_producer = 200U;
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool start = false;
    std::vector<std::thread> producers;
    for (std::size_t producer = 0U; producer < producer_count; ++producer) {
        producers.emplace_back([&logger, &gate_mutex, &gate, &start, producer]() {
            {
                std::unique_lock<std::mutex> lock{gate_mutex};
                gate.wait(lock, [&start]() { return start; });
            }
            for (std::size_t index = 0U; index < records_per_producer; ++index) {
                logger->log(LogLevel::Info, "race", std::to_string(producer));
            }
        });
    }
    std::promise<Result<void>> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    std::thread shutdown_thread([&logger, &gate_mutex, &gate, &start,
                                 promise = std::move(shutdown_promise)]() mutable {
        {
            std::unique_lock<std::mutex> lock{gate_mutex};
            gate.wait(lock, [&start]() { return start; });
        }
        promise.set_value(logger->shutdown());
    });
    {
        std::lock_guard<std::mutex> lock{gate_mutex};
        start = true;
    }
    gate.notify_all();
    for (auto& producer : producers) {
        producer.join();
    }
    shutdown_thread.join();
    EXPECT_TRUE(shutdown_future.get().has_value());
    const auto stats = logger->stats();
    EXPECT_EQ(stats.accepted + stats.dropped,
              producer_count * records_per_producer);
    EXPECT_EQ(logger->state(), AsyncLoggerState::Stopped);
}

TEST(AsyncLoggerTest, FlushAndShutdownRaceCompletesWithoutDeadlock) {
    auto logger_result = make_logger(options_for_tests(), std::make_unique<CapturingSink>());
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    logger->log(LogLevel::Info, "race", "record");

    std::mutex gate_mutex;
    std::condition_variable gate;
    bool start = false;
    Result<void> flush_result = Result<void>::success();
    Result<void> shutdown_result = Result<void>::success();
    std::thread flush_thread([&]() {
        std::unique_lock<std::mutex> lock{gate_mutex};
        gate.wait(lock, [&start]() { return start; });
        lock.unlock();
        flush_result = logger->flush();
    });
    std::thread shutdown_thread([&]() {
        std::unique_lock<std::mutex> lock{gate_mutex};
        gate.wait(lock, [&start]() { return start; });
        lock.unlock();
        shutdown_result = logger->shutdown();
    });
    {
        std::lock_guard<std::mutex> lock{gate_mutex};
        start = true;
    }
    gate.notify_all();
    flush_thread.join();
    shutdown_thread.join();
    EXPECT_TRUE(flush_result.has_value());
    EXPECT_TRUE(shutdown_result.has_value());
    EXPECT_EQ(logger->state(), AsyncLoggerState::Stopped);
}

TEST(AsyncLoggerTest, ConcurrentShutdownCallsHaveOneJoiner) {
    auto logger_result = make_logger(options_for_tests(), std::make_unique<CapturingSink>());
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    for (std::size_t index = 0U; index < 32U; ++index) {
        logger->log(LogLevel::Info, "shutdown", "drain");
    }

    std::vector<std::thread> callers;
    std::vector<int> results(8U, 0);
    for (std::size_t index = 0U; index < results.size(); ++index) {
        callers.emplace_back([&logger, &results, index]() {
            results[index] = logger->shutdown().has_value() ? 1 : 0;
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    for (const int result : results) {
        EXPECT_EQ(result, 1);
    }
    EXPECT_EQ(logger->state(), AsyncLoggerState::Stopped);
}

TEST(AsyncLoggerTest, ConcurrentProducersAreDrainedExactlyOnce) {
    auto options = AsyncLoggerOptions::create(2048U, 0U, 32U, 64U, 256U, 1024U, LogLevel::Trace);
    ASSERT_TRUE(options.has_value());
    auto sink = std::make_unique<CapturingSink>();
    auto* sink_ptr = sink.get();
    auto logger_result = make_logger(options.value(), std::move(sink));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());

    constexpr std::size_t producer_count = 8U;
    constexpr std::size_t records_per_producer = 100U;
    std::vector<std::thread> producers;
    for (std::size_t producer = 0U; producer < producer_count; ++producer) {
        producers.emplace_back([&logger, producer]() {
            for (std::size_t index = 0U; index < records_per_producer; ++index) {
                logger->log(LogLevel::Info, "producer", std::to_string(producer));
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    ASSERT_TRUE(logger->shutdown().has_value());
    EXPECT_EQ(logger->stats().accepted, producer_count * records_per_producer);
    EXPECT_EQ(sink_ptr->size(), producer_count * records_per_producer);
}

TEST(AsyncLoggerTest, WriterExceptionIsContainedAndReported) {
    auto logger_result = make_logger(options_for_tests(), std::make_unique<ThrowingSink>());
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    logger->log(LogLevel::Error, "exception", "payload");
    const auto shutdown_result = logger->shutdown();
    EXPECT_FALSE(shutdown_result.has_value());
    EXPECT_EQ(shutdown_result.error().code, ErrorCode::InternalError);
    EXPECT_GE(logger->stats().sink_failures, 1U);
}

TEST(AsyncLoggerTest, AllFailingSinksStillDrainAndExposeFailure) {
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(std::make_unique<ThrowingSink>());
    sinks.push_back(std::make_unique<ThrowingSink>());
    auto logger_result = AsyncLogger::create(options_for_tests(), std::move(sinks));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    logger->log(LogLevel::Error, "exception", "payload");
    const auto shutdown_result = logger->shutdown();
    EXPECT_FALSE(shutdown_result.has_value());
    EXPECT_EQ(logger->state(), AsyncLoggerState::Stopped);
    EXPECT_GE(logger->stats().sink_failures, 2U);
}

TEST(AsyncLoggerTest, SinkFailureDoesNotStopOtherSinksOrLaterRecords) {
    auto healthy_sink = std::make_unique<CapturingSink>();
    auto* healthy_sink_ptr = healthy_sink.get();
    std::vector<std::unique_ptr<ILogSink>> sinks;
    sinks.push_back(std::make_unique<ThrowingSink>());
    sinks.push_back(std::move(healthy_sink));
    auto logger_result = AsyncLogger::create(options_for_tests(), std::move(sinks));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    logger->log(LogLevel::Info, "mixed", "first");
    logger->log(LogLevel::Info, "mixed", "second");
    const auto shutdown_result = logger->shutdown();
    EXPECT_FALSE(shutdown_result.has_value());
    EXPECT_EQ(healthy_sink_ptr->size(), 2U);
    EXPECT_GE(logger->stats().sink_failures, 2U);
}

TEST(AsyncLoggerTest, FlushFailureIsObservableWithoutStoppingWriterEarly) {
    auto logger_result = make_logger(options_for_tests(), std::make_unique<FlushFailingSink>());
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    logger->log(LogLevel::Info, "flush", "record");
    const auto flush_result = logger->flush();
    EXPECT_FALSE(flush_result.has_value());
    EXPECT_GE(logger->stats().sink_failures, 1U);
    const auto shutdown_result = logger->shutdown();
    EXPECT_FALSE(shutdown_result.has_value());
    EXPECT_EQ(logger->state(), AsyncLoggerState::Stopped);
}

TEST(AsyncLoggerTest, WriterCannotJoinItself) {
    auto sink = std::make_unique<SelfShutdownSink>();
    auto* sink_ptr = sink.get();
    auto logger_result = make_logger(options_for_tests(), std::move(sink));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    sink_ptr->set_logger(logger.get());
    logger->log(LogLevel::Info, "self", "shutdown");
    ASSERT_TRUE(logger->shutdown().has_value());
    ASSERT_FALSE(sink_ptr->self_result().has_value());
    EXPECT_EQ(sink_ptr->self_result().error().code, ErrorCode::InvalidState);
}

TEST(AsyncLoggerTest, ConsoleSinkReceivesFormattedRecords) {
    std::ostringstream output;
    auto logger_result = make_logger(options_for_tests(), std::make_unique<ConsoleSink>(output));
    ASSERT_TRUE(logger_result.has_value());
    auto logger = std::move(logger_result.value());
    logger->log(LogLevel::Info, "console", "record");
    ASSERT_TRUE(logger->shutdown().has_value());
    EXPECT_NE(output.str().find("[INFO] [component=console]"), std::string::npos);
    EXPECT_NE(output.str().find("record"), std::string::npos);
}

}  // namespace
}  // namespace iaisf
