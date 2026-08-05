#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "iaisf/core/result.hpp"

namespace iaisf {

/** The concrete kind of a registered metric. */
enum class MetricType {
    Counter,
    Gauge,
    Histogram,
};

struct CounterSnapshot {
    std::string name;
    std::uint64_t value{0U};
};

struct GaugeSnapshot {
    std::string name;
    std::int64_t value{0};
};

struct HistogramSnapshot {
    std::string name;
    std::uint64_t count{0U};
    double sum{0.0};
    double min{0.0};
    double max{0.0};
};

/** A point-in-time copy of all metrics in a registry. */
struct MetricsSnapshot {
    std::vector<CounterSnapshot> counters;
    std::vector<GaugeSnapshot> gauges;
    std::vector<HistogramSnapshot> histograms;
};

/** Common read-only identity shared by all metric implementations. */
class Metric {
public:
    virtual ~Metric() = default;

    [[nodiscard]] const std::string& name() const noexcept {
        return name_;
    }

    [[nodiscard]] MetricType type() const noexcept {
        return type_;
    }

protected:
    Metric(std::string name, MetricType type)
        : name_(std::move(name)), type_(type) {}

private:
    std::string name_;
    MetricType type_;
};

/** A monotonically increasing unsigned counter. */
class Counter final : public Metric {
public:
    explicit Counter(std::string name);

    void increment(std::uint64_t amount = 1U) noexcept;
    void add(std::uint64_t amount) noexcept;

    [[nodiscard]] std::uint64_t snapshot() const noexcept;
    [[nodiscard]] CounterSnapshot snapshot_value() const;

private:
    std::atomic<std::uint64_t> value_{0U};
};

/** A signed value that may move in either direction. */
class Gauge final : public Metric {
public:
    explicit Gauge(std::string name);

    void set(std::int64_t value) noexcept;
    void increment(std::int64_t amount = 1) noexcept;
    void decrement(std::int64_t amount = 1) noexcept;

    [[nodiscard]] std::int64_t snapshot() const noexcept;
    [[nodiscard]] GaugeSnapshot snapshot_value() const;

private:
    std::atomic<std::int64_t> value_{0};
};

/** A mutex-protected first-generation numeric histogram. */
class Histogram final : public Metric {
public:
    explicit Histogram(std::string name);

    /** Non-finite values are ignored because they cannot form useful bounds. */
    void record(double value) noexcept;

    [[nodiscard]] HistogramSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    std::uint64_t count_{0U};
    double sum_{0.0};
    double min_{0.0};
    double max_{0.0};
};

/**
 * Owns a process-local collection of metrics.
 *
 * The registry is intentionally an ordinary object: callers decide its
 * lifetime (Application will own it when metrics are integrated). Returned
 * shared pointers keep a metric alive while a caller is using it; the
 * registry itself remains the authoritative owner of the name/type mapping.
 */
class MetricsRegistry final {
public:
    static constexpr std::size_t kMaximumNameBytes = 128U;

    MetricsRegistry() = default;
    ~MetricsRegistry() = default;

    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;
    MetricsRegistry(MetricsRegistry&&) = delete;
    MetricsRegistry& operator=(MetricsRegistry&&) = delete;

    [[nodiscard]] static bool is_valid_name(std::string_view name) noexcept;

    [[nodiscard]] Result<std::shared_ptr<Counter>> create_counter(
        std::string name);
    [[nodiscard]] Result<std::shared_ptr<Gauge>> create_gauge(
        std::string name);
    [[nodiscard]] Result<std::shared_ptr<Histogram>> create_histogram(
        std::string name);

    [[nodiscard]] Result<std::shared_ptr<Metric>> get(
        std::string_view name) const;
    [[nodiscard]] Result<std::shared_ptr<Counter>> get_counter(
        std::string_view name) const;
    [[nodiscard]] Result<std::shared_ptr<Gauge>> get_gauge(
        std::string_view name) const;
    [[nodiscard]] Result<std::shared_ptr<Histogram>> get_histogram(
        std::string_view name) const;

    [[nodiscard]] Result<MetricsSnapshot> snapshot() const;
    [[nodiscard]] Result<MetricsSnapshot> snapshot_all() const {
        return snapshot();
    }

    [[nodiscard]] std::size_t size() const noexcept;

private:
    [[nodiscard]] Result<std::shared_ptr<Metric>> insert(
        std::string name,
        MetricType type,
        std::shared_ptr<Metric> metric);

    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Metric>> metrics_;
};

}  // namespace iaisf
