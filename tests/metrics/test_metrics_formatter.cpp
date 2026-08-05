#include <string>

#include <gtest/gtest.h>

#include "iaisf/metrics/metrics.hpp"
#include "iaisf/metrics/prometheus_formatter.hpp"

namespace {

TEST(MetricsFormatterTest, EmitsPrometheusTextForAllMetricKinds) {
    iaisf::MetricsRegistry registry;
    const auto requests = registry.create_counter("requests_total");
    const auto active = registry.create_gauge("active_connections");
    const auto latency = registry.create_histogram("request_latency");
    ASSERT_TRUE(requests);
    ASSERT_TRUE(active);
    ASSERT_TRUE(latency);

    requests.value()->add(3U);
    active.value()->set(2);
    latency.value()->record(1.25);
    latency.value()->record(3.5);

    const auto snapshot = registry.snapshot();
    ASSERT_TRUE(snapshot);
    const auto formatted = iaisf::metrics::PrometheusFormatter::format(
        snapshot.value());
    ASSERT_TRUE(formatted);
    const std::string& text = formatted.value();

    EXPECT_NE(text.find("# TYPE requests_total counter\n"), std::string::npos);
    EXPECT_NE(text.find("requests_total 3\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE active_connections gauge\n"), std::string::npos);
    EXPECT_NE(text.find("active_connections 2\n"), std::string::npos);
    EXPECT_NE(text.find("# TYPE request_latency histogram\n"), std::string::npos);
    EXPECT_NE(text.find("request_latency_count 2\n"), std::string::npos);
    EXPECT_NE(text.find("request_latency_sum 4.75\n"), std::string::npos);
    EXPECT_NE(text.find("request_latency_min 1.25\n"), std::string::npos);
    EXPECT_NE(text.find("request_latency_max 3.5\n"), std::string::npos);
    EXPECT_EQ(text.find('{'), std::string::npos);
}

TEST(MetricsFormatterTest, EmptySnapshotIsEmpty) {
    const auto formatted = iaisf::metrics::PrometheusFormatter::format(
        iaisf::MetricsSnapshot{});
    ASSERT_TRUE(formatted);
    EXPECT_TRUE(formatted.value().empty());
}

}  // namespace
