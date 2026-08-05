#include "iaisf/metrics/metrics.hpp"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace iaisf {
namespace {

class StartGate {
public:
    explicit StartGate(const std::size_t participants)
        : participants_(participants) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock{mutex_};
        ++arrived_;
        if (arrived_ == participants_) {
            open_ = true;
            condition_.notify_all();
        } else {
            condition_.wait(lock, [this]() { return open_; });
        }
    }

private:
    const std::size_t participants_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t arrived_{0U};
    bool open_{false};
};

TEST(MetricsRegistryTest, CreatesGetsAndSnapshotsAllMetricKinds) {
    MetricsRegistry registry;
    auto counter_result = registry.create_counter("requests_total");
    auto gauge_result = registry.create_gauge("active_workers");
    auto histogram_result = registry.create_histogram("request_latency");
    ASSERT_TRUE(counter_result.has_value());
    ASSERT_TRUE(gauge_result.has_value());
    ASSERT_TRUE(histogram_result.has_value());

    counter_result.value()->add(3U);
    gauge_result.value()->set(4);
    histogram_result.value()->record(2.5);

    ASSERT_TRUE(registry.get("requests_total").has_value());
    ASSERT_TRUE(registry.get_counter("requests_total").has_value());
    ASSERT_TRUE(registry.get_gauge("active_workers").has_value());
    ASSERT_TRUE(registry.get_histogram("request_latency").has_value());
    EXPECT_EQ(registry.size(), 3U);

    const auto snapshot_result = registry.snapshot_all();
    ASSERT_TRUE(snapshot_result.has_value());
    const auto& snapshot = snapshot_result.value();
    ASSERT_EQ(snapshot.counters.size(), 1U);
    ASSERT_EQ(snapshot.gauges.size(), 1U);
    ASSERT_EQ(snapshot.histograms.size(), 1U);
    EXPECT_EQ(snapshot.counters.front().value, 3U);
    EXPECT_EQ(snapshot.gauges.front().value, 4);
    EXPECT_DOUBLE_EQ(snapshot.histograms.front().sum, 2.5);
}

TEST(MetricsRegistryTest, DuplicateMetricNameIsRejectedAcrossTypes) {
    MetricsRegistry registry;
    ASSERT_TRUE(registry.create_counter("same_name").has_value());
    const auto duplicate = registry.create_gauge("same_name");
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(MetricsRegistryTest, InvalidMetricNamesAreRejected) {
    MetricsRegistry registry;
    for (const std::string name : {"", "9requests", "request-count", "request count",
                                   "请求"}) {
        const auto result = registry.create_counter(name);
        ASSERT_FALSE(result.has_value()) << name;
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
    EXPECT_TRUE(MetricsRegistry::is_valid_name("valid_name_1"));
}

TEST(MetricsRegistryTest, CounterSupportsConcurrentAtomicUpdates) {
    MetricsRegistry registry;
    auto result = registry.create_counter("events_total");
    ASSERT_TRUE(result.has_value());
    const auto counter = result.value();

    constexpr std::size_t worker_count = 8U;
    constexpr std::size_t increments = 10000U;
    StartGate gate{worker_count};
    std::vector<std::thread> workers;
    for (std::size_t worker = 0U; worker < worker_count; ++worker) {
        workers.emplace_back([counter, &gate]() {
            gate.arrive_and_wait();
            for (std::size_t index = 0U; index < increments; ++index) {
                counter->increment();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(counter->snapshot(), worker_count * increments);
}

TEST(MetricsRegistryTest, GaugeSupportsConcurrentAtomicUpdates) {
    MetricsRegistry registry;
    auto result = registry.create_gauge("active_connections");
    ASSERT_TRUE(result.has_value());
    const auto gauge = result.value();
    gauge->set(10);

    constexpr std::size_t incrementers = 4U;
    constexpr std::size_t decrementers = 3U;
    constexpr std::size_t updates = 5000U;
    StartGate gate{incrementers + decrementers};
    std::vector<std::thread> workers;
    for (std::size_t worker = 0U; worker < incrementers; ++worker) {
        workers.emplace_back([gauge, &gate]() {
            gate.arrive_and_wait();
            for (std::size_t index = 0U; index < updates; ++index) {
                gauge->increment();
            }
        });
    }
    for (std::size_t worker = 0U; worker < decrementers; ++worker) {
        workers.emplace_back([gauge, &gate]() {
            gate.arrive_and_wait();
            for (std::size_t index = 0U; index < updates; ++index) {
                gauge->decrement();
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(gauge->snapshot(), 10 + static_cast<std::int64_t>(updates));
}

TEST(MetricsRegistryTest, HistogramTracksCountSumAndBounds) {
    MetricsRegistry registry;
    auto result = registry.create_histogram("latency_seconds");
    ASSERT_TRUE(result.has_value());
    const auto histogram = result.value();
    histogram->record(3.0);
    histogram->record(-1.5);
    histogram->record(2.0);
    histogram->record(std::numeric_limits<double>::quiet_NaN());
    histogram->record(std::numeric_limits<double>::infinity());

    const auto snapshot = histogram->snapshot();
    EXPECT_EQ(snapshot.count, 3U);
    EXPECT_DOUBLE_EQ(snapshot.sum, 3.5);
    EXPECT_DOUBLE_EQ(snapshot.min, -1.5);
    EXPECT_DOUBLE_EQ(snapshot.max, 3.0);
}

TEST(MetricsRegistryTest, SnapshotCanRunConcurrentlyWithMetricUpdates) {
    MetricsRegistry registry;
    auto counter_result = registry.create_counter("snapshot_total");
    auto gauge_result = registry.create_gauge("snapshot_gauge");
    auto histogram_result = registry.create_histogram("snapshot_histogram");
    ASSERT_TRUE(counter_result.has_value());
    ASSERT_TRUE(gauge_result.has_value());
    ASSERT_TRUE(histogram_result.has_value());
    const auto counter = counter_result.value();
    const auto gauge = gauge_result.value();
    const auto histogram = histogram_result.value();

    constexpr std::size_t writer_count = 4U;
    constexpr std::size_t reader_count = 2U;
    StartGate gate{writer_count + reader_count};
    std::atomic<bool> reader_failed{false};
    std::vector<std::thread> workers;
    for (std::size_t writer = 0U; writer < writer_count; ++writer) {
        workers.emplace_back([counter, gauge, histogram, &gate]() {
            gate.arrive_and_wait();
            for (std::size_t index = 0U; index < 2000U; ++index) {
                counter->increment();
                gauge->increment();
                histogram->record(static_cast<double>(index));
            }
        });
    }
    for (std::size_t reader = 0U; reader < reader_count; ++reader) {
        workers.emplace_back([&registry, &gate, &reader_failed]() {
            gate.arrive_and_wait();
            for (std::size_t index = 0U; index < 500U; ++index) {
                const auto snapshot = registry.snapshot();
                if (!snapshot.has_value() || snapshot.value().counters.size() != 1U ||
                    snapshot.value().gauges.size() != 1U ||
                    snapshot.value().histograms.size() != 1U) {
                    reader_failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_FALSE(reader_failed.load(std::memory_order_relaxed));
    EXPECT_EQ(counter->snapshot(), writer_count * 2000U);
    EXPECT_EQ(gauge->snapshot(), static_cast<std::int64_t>(writer_count * 2000U));
    EXPECT_EQ(histogram->snapshot().count, writer_count * 2000U);
}

}  // namespace
}  // namespace iaisf
