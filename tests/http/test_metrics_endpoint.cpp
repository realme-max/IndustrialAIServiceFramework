#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "iaisf/config/app_config.hpp"
#include "iaisf/http/builtin_routes.hpp"
#include "iaisf/http/http_request.hpp"
#include "iaisf/http/http_router.hpp"
#include "iaisf/metrics/metrics.hpp"

namespace {

std::filesystem::path write_config(const std::string& contents) {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto directory = std::filesystem::temp_directory_path() /
        ("iaisf-metrics-endpoint-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()) +
         "-" + std::to_string(sequence.fetch_add(1U)));
    std::filesystem::create_directories(directory);
    const auto path = directory / "config.json";
    std::ofstream output{path};
    output << contents;
    return path;
}

void expect_config_rejected(const std::string& contents) {
    const auto path = write_config(contents);
    const auto result = iaisf::load_app_config(path);
    std::error_code ignored;
    std::filesystem::remove_all(path.parent_path(), ignored);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::ConfigError);
}

iaisf::Result<iaisf::http::HttpResponse> dispatch_metrics(
    iaisf::MetricsRegistry& metrics,
    const std::string& endpoint) {
    iaisf::http::HttpRouter router;
    auto route = iaisf::http::register_metrics_route(
        router, metrics, endpoint);
    if (!route) {
        return iaisf::Result<iaisf::http::HttpResponse>::failure(
            std::move(route).error());
    }
    auto frozen = router.freeze();
    if (!frozen) {
        return iaisf::Result<iaisf::http::HttpResponse>::failure(
            std::move(frozen).error());
    }
    auto request = iaisf::http::HttpRequest::create(
        "GET", endpoint, {}, {}, true);
    if (!request) {
        return iaisf::Result<iaisf::http::HttpResponse>::failure(
            std::move(request).error());
    }
    return router.dispatch(request.value());
}

TEST(MetricsEndpointTest, ReturnsSnapshotWithPrometheusContentType) {
    iaisf::MetricsRegistry metrics;
    auto counter = metrics.create_counter("requests_total");
    ASSERT_TRUE(counter);
    counter.value()->increment(4U);

    auto response = dispatch_metrics(metrics, "/metrics");
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), iaisf::http::HttpStatus::Ok);
    const auto content_type = response.value().headers().front().value;
    EXPECT_EQ(content_type, "text/plain; version=0.0.4");
    EXPECT_NE(response.value().body().find("requests_total 4\n"),
              std::string::npos);
}

TEST(MetricsEndpointTest, DisabledConfigurationDoesNotEnableMetricsRoute) {
    iaisf::MetricsRegistry metrics;
    auto config = iaisf::default_app_config();
    config.metrics.enabled = false;
    const auto config_path = write_config(
        R"({"metrics":{"enabled":false,"endpoint":"/internal-metrics"}})");
    const auto loaded = iaisf::load_app_config(config_path);
    std::error_code ignored;
    std::filesystem::remove_all(config_path.parent_path(), ignored);
    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded.value().metrics.enabled);
    EXPECT_EQ(loaded.value().metrics.endpoint, "/internal-metrics");

    iaisf::http::HttpRouter router;
    if (config.metrics.enabled) {
        ASSERT_TRUE(iaisf::http::register_metrics_route(
            router, metrics, config.metrics.endpoint));
    }
    auto frozen = router.freeze();
    ASSERT_TRUE(frozen);
    auto request = iaisf::http::HttpRequest::create(
        "GET", "/metrics", {}, {}, true);
    ASSERT_TRUE(request);
    const auto response = router.dispatch(request.value());
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), iaisf::http::HttpStatus::NotFound);
}

TEST(MetricsEndpointTest, InvalidMetricsConfigurationIsRejected) {
    iaisf::AppConfig config = iaisf::default_app_config();
    config.metrics.endpoint = "metrics";
    auto validation = iaisf::validate_app_config(config);
    ASSERT_FALSE(validation);
    EXPECT_EQ(validation.error().code, iaisf::ErrorCode::ConfigError);

    config = iaisf::default_app_config();
    config.metrics.endpoint.assign(
        iaisf::kMaxMetricsEndpointBytes + 1U, 'x');
    config.metrics.endpoint.front() = '/';
    validation = iaisf::validate_app_config(config);
    ASSERT_FALSE(validation);
    EXPECT_EQ(validation.error().code, iaisf::ErrorCode::ConfigError);
}

TEST(MetricsEndpointTest, RejectsUnknownAndWrongTypeMetricsConfigFields) {
    expect_config_rejected(R"({"metrics":{"unknown":true}})");
    expect_config_rejected(R"({"metrics":{"enabled":"true"}})");
    expect_config_rejected(R"({"metrics":{"endpoint":17}})");
}

}  // namespace
