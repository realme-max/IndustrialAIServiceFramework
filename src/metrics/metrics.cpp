#include "iaisf/metrics/metrics.hpp"

#include <cmath>
#include <new>
#include <utility>

namespace iaisf {

Counter::Counter(std::string name)
    : Metric(std::move(name), MetricType::Counter) {}

void Counter::increment(const std::uint64_t amount) noexcept {
    value_.fetch_add(amount, std::memory_order_relaxed);
}

void Counter::add(const std::uint64_t amount) noexcept {
    increment(amount);
}

std::uint64_t Counter::snapshot() const noexcept {
    return value_.load(std::memory_order_relaxed);
}

CounterSnapshot Counter::snapshot_value() const {
    return CounterSnapshot{name(), snapshot()};
}

Gauge::Gauge(std::string name)
    : Metric(std::move(name), MetricType::Gauge) {}

void Gauge::set(const std::int64_t value) noexcept {
    value_.store(value, std::memory_order_relaxed);
}

void Gauge::increment(const std::int64_t amount) noexcept {
    value_.fetch_add(amount, std::memory_order_relaxed);
}

void Gauge::decrement(const std::int64_t amount) noexcept {
    value_.fetch_sub(amount, std::memory_order_relaxed);
}

std::int64_t Gauge::snapshot() const noexcept {
    return value_.load(std::memory_order_relaxed);
}

GaugeSnapshot Gauge::snapshot_value() const {
    return GaugeSnapshot{name(), snapshot()};
}

Histogram::Histogram(std::string name)
    : Metric(std::move(name), MetricType::Histogram) {}

void Histogram::record(const double value) noexcept {
    if (!std::isfinite(value)) {
        return;
    }

    std::lock_guard<std::mutex> lock{mutex_};
    if (count_ == 0U) {
        min_ = value;
        max_ = value;
    } else {
        if (value < min_) {
            min_ = value;
        }
        if (value > max_) {
            max_ = value;
        }
    }
    ++count_;
    sum_ += value;
}

HistogramSnapshot Histogram::snapshot() const {
    std::lock_guard<std::mutex> lock{mutex_};
    return HistogramSnapshot{name(), count_, sum_, min_, max_};
}

bool MetricsRegistry::is_valid_name(const std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaximumNameBytes) {
        return false;
    }

    const auto is_letter = [](const unsigned char value) noexcept {
        return (value >= static_cast<unsigned char>('a') &&
                value <= static_cast<unsigned char>('z')) ||
               (value >= static_cast<unsigned char>('A') &&
                value <= static_cast<unsigned char>('Z'));
    };
    const auto is_digit = [](const unsigned char value) noexcept {
        return value >= static_cast<unsigned char>('0') &&
               value <= static_cast<unsigned char>('9');
    };

    const auto first = static_cast<unsigned char>(name.front());
    if (!is_letter(first) && first != static_cast<unsigned char>('_')) {
        return false;
    }
    for (const char raw : name.substr(1U)) {
        const auto value = static_cast<unsigned char>(raw);
        if (!is_letter(value) && !is_digit(value) &&
            value != static_cast<unsigned char>('_')) {
            return false;
        }
    }
    return true;
}

Result<std::shared_ptr<Metric>> MetricsRegistry::insert(
    std::string name,
    const MetricType type,
    std::shared_ptr<Metric> metric) {
    if (!is_valid_name(name)) {
        return Result<std::shared_ptr<Metric>>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid metric name"));
    }

    std::lock_guard<std::mutex> lock{mutex_};
    if (metrics_.find(name) != metrics_.end()) {
        return Result<std::shared_ptr<Metric>>::failure(
            make_error(ErrorCode::InvalidArgument, "duplicate metric name"));
    }
    if (metric == nullptr || metric->type() != type) {
        return Result<std::shared_ptr<Metric>>::failure(
            make_error(ErrorCode::InternalError, "metric type mismatch"));
    }
    metrics_.emplace(std::move(name), metric);
    return Result<std::shared_ptr<Metric>>::success(std::move(metric));
}

Result<std::shared_ptr<Counter>> MetricsRegistry::create_counter(
    std::string name) {
    if (!is_valid_name(name)) {
        return Result<std::shared_ptr<Counter>>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid metric name"));
    }
    try {
        auto metric = std::make_shared<Counter>(name);
        auto inserted = insert(
            std::move(name), MetricType::Counter,
            std::static_pointer_cast<Metric>(metric));
        if (!inserted) {
            return Result<std::shared_ptr<Counter>>::failure(inserted.error());
        }
        return Result<std::shared_ptr<Counter>>::success(std::move(metric));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<Counter>>::failure(
            make_error(ErrorCode::ResourceExhausted, "metric allocation failed"));
    } catch (...) {
        return Result<std::shared_ptr<Counter>>::failure(
            make_error(ErrorCode::InternalError, "metric creation failed"));
    }
}

Result<std::shared_ptr<Gauge>> MetricsRegistry::create_gauge(std::string name) {
    if (!is_valid_name(name)) {
        return Result<std::shared_ptr<Gauge>>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid metric name"));
    }
    try {
        auto metric = std::make_shared<Gauge>(name);
        auto inserted = insert(
            std::move(name), MetricType::Gauge,
            std::static_pointer_cast<Metric>(metric));
        if (!inserted) {
            return Result<std::shared_ptr<Gauge>>::failure(inserted.error());
        }
        return Result<std::shared_ptr<Gauge>>::success(std::move(metric));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<Gauge>>::failure(
            make_error(ErrorCode::ResourceExhausted, "metric allocation failed"));
    } catch (...) {
        return Result<std::shared_ptr<Gauge>>::failure(
            make_error(ErrorCode::InternalError, "metric creation failed"));
    }
}

Result<std::shared_ptr<Histogram>> MetricsRegistry::create_histogram(
    std::string name) {
    if (!is_valid_name(name)) {
        return Result<std::shared_ptr<Histogram>>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid metric name"));
    }
    try {
        auto metric = std::make_shared<Histogram>(name);
        auto inserted = insert(
            std::move(name), MetricType::Histogram,
            std::static_pointer_cast<Metric>(metric));
        if (!inserted) {
            return Result<std::shared_ptr<Histogram>>::failure(inserted.error());
        }
        return Result<std::shared_ptr<Histogram>>::success(std::move(metric));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<Histogram>>::failure(
            make_error(ErrorCode::ResourceExhausted, "metric allocation failed"));
    } catch (...) {
        return Result<std::shared_ptr<Histogram>>::failure(
            make_error(ErrorCode::InternalError, "metric creation failed"));
    }
}

Result<std::shared_ptr<Metric>> MetricsRegistry::get(
    const std::string_view name) const {
    if (!is_valid_name(name)) {
        return Result<std::shared_ptr<Metric>>::failure(
            make_error(ErrorCode::InvalidArgument, "invalid metric name"));
    }
    try {
        std::lock_guard<std::mutex> lock{mutex_};
        const auto found = metrics_.find(std::string{name});
        if (found == metrics_.end()) {
            return Result<std::shared_ptr<Metric>>::failure(
                make_error(ErrorCode::NotFound, "metric not found"));
        }
        return Result<std::shared_ptr<Metric>>::success(found->second);
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<Metric>>::failure(
            make_error(ErrorCode::ResourceExhausted, "metric lookup allocation failed"));
    } catch (...) {
        return Result<std::shared_ptr<Metric>>::failure(
            make_error(ErrorCode::InternalError, "metric lookup failed"));
    }
}

Result<std::shared_ptr<Counter>> MetricsRegistry::get_counter(
    const std::string_view name) const {
    const auto metric = get(name);
    if (!metric) {
        return Result<std::shared_ptr<Counter>>::failure(metric.error());
    }
    auto counter = std::dynamic_pointer_cast<Counter>(metric.value());
    if (counter == nullptr) {
        return Result<std::shared_ptr<Counter>>::failure(
            make_error(ErrorCode::InvalidArgument, "metric type mismatch"));
    }
    return Result<std::shared_ptr<Counter>>::success(std::move(counter));
}

Result<std::shared_ptr<Gauge>> MetricsRegistry::get_gauge(
    const std::string_view name) const {
    const auto metric = get(name);
    if (!metric) {
        return Result<std::shared_ptr<Gauge>>::failure(metric.error());
    }
    auto gauge = std::dynamic_pointer_cast<Gauge>(metric.value());
    if (gauge == nullptr) {
        return Result<std::shared_ptr<Gauge>>::failure(
            make_error(ErrorCode::InvalidArgument, "metric type mismatch"));
    }
    return Result<std::shared_ptr<Gauge>>::success(std::move(gauge));
}

Result<std::shared_ptr<Histogram>> MetricsRegistry::get_histogram(
    const std::string_view name) const {
    const auto metric = get(name);
    if (!metric) {
        return Result<std::shared_ptr<Histogram>>::failure(metric.error());
    }
    auto histogram = std::dynamic_pointer_cast<Histogram>(metric.value());
    if (histogram == nullptr) {
        return Result<std::shared_ptr<Histogram>>::failure(
            make_error(ErrorCode::InvalidArgument, "metric type mismatch"));
    }
    return Result<std::shared_ptr<Histogram>>::success(std::move(histogram));
}

Result<MetricsSnapshot> MetricsRegistry::snapshot() const {
    try {
        MetricsSnapshot result;
        std::lock_guard<std::mutex> lock{mutex_};
        for (const auto& [name, metric] : metrics_) {
            (void)name;
            if (const auto counter = std::dynamic_pointer_cast<Counter>(metric)) {
                result.counters.push_back(counter->snapshot_value());
            } else if (const auto gauge = std::dynamic_pointer_cast<Gauge>(metric)) {
                result.gauges.push_back(gauge->snapshot_value());
            } else if (const auto histogram =
                           std::dynamic_pointer_cast<Histogram>(metric)) {
                result.histograms.push_back(histogram->snapshot());
            } else {
                return Result<MetricsSnapshot>::failure(
                    make_error(ErrorCode::InternalError, "unknown metric type"));
            }
        }
        return Result<MetricsSnapshot>::success(std::move(result));
    } catch (const std::bad_alloc&) {
        return Result<MetricsSnapshot>::failure(
            make_error(ErrorCode::ResourceExhausted, "metrics snapshot allocation failed"));
    } catch (...) {
        return Result<MetricsSnapshot>::failure(
            make_error(ErrorCode::InternalError, "metrics snapshot failed"));
    }
}

std::size_t MetricsRegistry::size() const noexcept {
    std::lock_guard<std::mutex> lock{mutex_};
    return metrics_.size();
}

}  // namespace iaisf
