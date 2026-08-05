#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "iaisf/logging/async_logger.hpp"
#include "iaisf/logging/log_sink.hpp"
#include "iaisf/metrics/metrics.hpp"

namespace {

class BlockingSink final : public iaisf::ILogSink {
public:
    iaisf::Result<void> write(
        const iaisf::LogRecord&,
        std::string_view) override {
        std::unique_lock<std::mutex> lock{mutex_};
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
        return iaisf::Result<void>::success();
    }

    iaisf::Result<void> flush() override {
        return iaisf::Result<void>::success();
    }

    void wait_until_entered() {
        std::unique_lock<std::mutex> lock{mutex_};
        ASSERT_TRUE(changed_.wait_for(
            lock,
            std::chrono::seconds(5),
            [this] { return entered_; }));
    }

    void release() {
        std::lock_guard<std::mutex> lock{mutex_};
        released_ = true;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{false};
    bool released_{false};
};

class FailingSink final : public iaisf::ILogSink {
public:
    iaisf::Result<void> write(
        const iaisf::LogRecord&,
        std::string_view) override {
        return iaisf::Result<void>::failure(
            iaisf::make_error(iaisf::ErrorCode::IoError, "sink failed"));
    }

    iaisf::Result<void> flush() override {
        return iaisf::Result<void>::success();
    }
};

iaisf::AsyncLoggerOptions metrics_test_options(
    std::size_t queue_capacity,
    std::size_t batch_size) {
    auto result = iaisf::AsyncLoggerOptions::create(
        queue_capacity,
        0U,
        batch_size,
        128U,
        4096U,
        8192U,
        iaisf::LogLevel::Info);
    EXPECT_TRUE(result);
    auto options = std::move(result).value();
    options.flush_interval_ms = 0U;
    return options;
}

TEST(LoggerMetricsTest, TracksAcceptedAndDroppedRecords) {
    iaisf::MetricsRegistry metrics;
    auto sink = std::make_unique<BlockingSink>();
    auto* sink_ptr = sink.get();
    std::vector<std::unique_ptr<iaisf::ILogSink>> sinks;
    sinks.push_back(std::move(sink));
    auto logger_result = iaisf::AsyncLogger::create(
        metrics_test_options(1U, 1U), std::move(sinks), &metrics);
    ASSERT_TRUE(logger_result);
    auto logger = std::move(logger_result).value();

    logger->log(iaisf::LogLevel::Info, "metrics", "first");
    sink_ptr->wait_until_entered();
    logger->log(iaisf::LogLevel::Info, "metrics", "queued");
    logger->log(iaisf::LogLevel::Info, "metrics", "dropped");

    sink_ptr->release();
    ASSERT_TRUE(logger->flush());
    ASSERT_TRUE(logger->shutdown());

    const auto accepted = metrics.get_counter("logger_records_accepted_total");
    const auto dropped = metrics.get_counter("logger_records_dropped_total");
    const auto failures = metrics.get_counter("logger_sink_failures_total");
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(dropped);
    ASSERT_TRUE(failures);
    EXPECT_EQ(accepted.value()->snapshot(), 2U);
    EXPECT_EQ(dropped.value()->snapshot(), 1U);
    EXPECT_EQ(failures.value()->snapshot(), 0U);
}

TEST(LoggerMetricsTest, TracksSinkFailuresWithoutStoppingProducer) {
    iaisf::MetricsRegistry metrics;
    std::vector<std::unique_ptr<iaisf::ILogSink>> sinks;
    sinks.push_back(std::make_unique<FailingSink>());
    auto logger_result = iaisf::AsyncLogger::create(
        metrics_test_options(4U, 1U), std::move(sinks), &metrics);
    ASSERT_TRUE(logger_result);
    auto logger = std::move(logger_result).value();

    logger->log(iaisf::LogLevel::Info, "metrics", "failure");
    const auto flush = logger->flush();
    EXPECT_FALSE(flush);
    const auto shutdown = logger->shutdown();
    EXPECT_FALSE(shutdown);

    const auto accepted = metrics.get_counter("logger_records_accepted_total");
    const auto dropped = metrics.get_counter("logger_records_dropped_total");
    const auto failures = metrics.get_counter("logger_sink_failures_total");
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(dropped);
    ASSERT_TRUE(failures);
    EXPECT_EQ(accepted.value()->snapshot(), 1U);
    EXPECT_EQ(dropped.value()->snapshot(), 0U);
    EXPECT_GE(failures.value()->snapshot(), 1U);
}

}  // namespace
