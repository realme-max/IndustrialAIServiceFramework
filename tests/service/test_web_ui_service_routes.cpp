#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/unique_fd.hpp"
#include "iaisf/service/industrial_ai_service.hpp"
#include "iaisf/service/service_options.hpp"

namespace iaisf::service {
namespace {

ApplicationRuntimeOptions enabled_applications() {
    ApplicationRuntimeOptions applications;
    applications.enabled = true;
    applications.artifact_root = "artifacts";
    applications.scratch_root = "scratch";
    applications.output_root = "outputs";
    applications.ptv2.executable = "ptv2";
    applications.ptv2.engine = "engine";
    applications.ptv2.plugin = "plugin";
    applications.weld_agent.python_executable = "python";
    applications.weld_agent.orchestrator = "orchestrator.py";
    applications.weld_agent.tool_config = "tool.json";
    applications.weld_agent.project_root = "weld-agent";
    return applications;
}

class TestLogger final : public ILogger {
public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};

struct WireResponse {
    int status{0};
    std::string body;
};

Result<net::UniqueFd> connect_client(const net::tcp::Ipv4Endpoint& endpoint) {
    net::UniqueFd descriptor{
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
    if (!descriptor.valid()) {
        return Result<net::UniqueFd>::failure(make_error(
            ErrorCode::IoError, "test client socket failed"));
    }
    const timeval timeout{5, 0};
    if (::setsockopt(descriptor.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     static_cast<socklen_t>(sizeof(timeout))) != 0 ||
        ::setsockopt(descriptor.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     static_cast<socklen_t>(sizeof(timeout))) != 0) {
        return Result<net::UniqueFd>::failure(make_error(
            ErrorCode::IoError, "test client timeout setup failed"));
    }
    const auto address = endpoint.to_sockaddr();
    if (::connect(descriptor.get(), reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(sizeof(address))) != 0) {
        return Result<net::UniqueFd>::failure(make_error(
            ErrorCode::IoError, "test client connect failed"));
    }
    return Result<net::UniqueFd>::success(std::move(descriptor));
}

Result<WireResponse> get_once(
    const net::tcp::Ipv4Endpoint& endpoint,
    const std::string_view path) {
    auto socket = connect_client(endpoint);
    if (!socket) return Result<WireResponse>::failure(socket.error());
    const std::string request =
        "GET " + std::string(path) +
        " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    std::size_t sent = 0U;
    while (sent < request.size()) {
        const auto count = ::send(socket.value().get(), request.data() + sent,
                                  request.size() - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return Result<WireResponse>::failure(make_error(
            ErrorCode::IoError, "test client send failed"));
    }
    std::string bytes;
    char buffer[4096];
    for (;;) {
        const auto count = ::recv(socket.value().get(), buffer, sizeof(buffer), 0);
        if (count > 0) {
            bytes.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        return Result<WireResponse>::failure(make_error(
            ErrorCode::IoError, "test client receive failed"));
    }
    const auto separator = bytes.find("\r\n\r\n");
    if (separator == std::string::npos || bytes.size() < 12U ||
        bytes.compare(0U, 9U, "HTTP/1.1 ") != 0) {
        return Result<WireResponse>::failure(make_error(
            ErrorCode::InvalidArgument, "invalid test HTTP response"));
    }
    WireResponse response;
    try {
        response.status = std::stoi(bytes.substr(9U, 3U));
        response.body = bytes.substr(separator + 4U);
    } catch (...) {
        return Result<WireResponse>::failure(make_error(
            ErrorCode::InvalidArgument, "invalid test HTTP status"));
    }
    return Result<WireResponse>::success(std::move(response));
}

ApplicationRuntimeOptions temporary_applications(
    const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "artifacts");
    std::filesystem::create_directories(root / "scratch");
    std::filesystem::create_directories(root / "outputs");
    auto options = enabled_applications();
    options.artifact_root = root / "artifacts";
    options.scratch_root = root / "scratch";
    options.output_root = root / "outputs";
    return options;
}

Result<ServiceOptions> options_with_applications(
    const std::int64_t routes,
    const bool metrics,
    const bool diagnostics,
    ApplicationRuntimeOptions applications) {
    const auto defaults = ServiceOptions::defaults();
    if (!defaults) return Result<ServiceOptions>::failure(defaults.error());
    const auto& base = defaults.value();
    const auto& http = base.http_limits();
    auto limits = http::HttpLimits::create(
        static_cast<std::int64_t>(http.max_request_line_bytes()),
        static_cast<std::int64_t>(http.max_method_bytes()),
        static_cast<std::int64_t>(http.max_target_bytes()),
        static_cast<std::int64_t>(http.max_header_line_bytes()),
        static_cast<std::int64_t>(http.max_header_bytes()),
        static_cast<std::int64_t>(http.max_header_count()),
        static_cast<std::int64_t>(http.max_body_bytes()),
        static_cast<std::int64_t>(http.max_response_body_bytes()), routes,
        static_cast<std::int64_t>(http.max_requests_per_dispatch()));
    if (!limits) return Result<ServiceOptions>::failure(limits.error());
    return ServiceOptions::create(
        base.tcp_options(), std::move(limits).value(), base.pool_options(),
        base.task_limits(), base.plugin_limits(), base.api_limits(),
        base.enable_echo(), base.enable_mock_vision(), base.http_header_timeout(),
        base.http_body_timeout(), metrics, base.metrics_endpoint(), diagnostics,
        base.diagnostics_endpoint(), base.dynamic_plugins(),
        std::move(applications));
}

Result<ServiceOptions> options_with_routes(
    const std::int64_t routes,
    const bool metrics,
    const bool diagnostics) {
    return options_with_applications(
        routes, metrics, diagnostics, enabled_applications());
}

TEST(ServiceWebUiRouteTest, ApplicationCapacityIncludesFourWebResources) {
    const auto valid = options_with_routes(17, false, false);
    ASSERT_TRUE(valid);

    const auto too_small = options_with_routes(16, false, false);
    ASSERT_FALSE(too_small);
    EXPECT_EQ(too_small.error().code, ErrorCode::InvalidArgument);
}

TEST(ServiceWebUiRouteTest, MetricsAndDiagnosticsAreIncludedInCapacity) {
    EXPECT_TRUE(options_with_routes(18, true, false));
    EXPECT_TRUE(options_with_routes(18, false, true));
    EXPECT_TRUE(options_with_routes(19, true, true));
    EXPECT_FALSE(options_with_routes(18, true, true));
}

Result<WireResponse> run_get(
    ServiceOptions options,
    const std::string_view path) {
    TestLogger logger;
    auto loop_result = net::EventLoop::create(logger, 128U, 256U);
    if (!loop_result) return Result<WireResponse>::failure(loop_result.error());
    auto loop = std::move(loop_result).value();
    auto service_result = IndustrialAiService::create(
        *loop, logger, net::tcp::Ipv4Endpoint::loopback(0U), std::move(options));
    if (!service_result) return Result<WireResponse>::failure(service_result.error());
    auto service = std::move(service_result).value();
    auto started = service->start();
    if (!started) return Result<WireResponse>::failure(started.error());
    const auto endpoint = service->local_endpoint();
    std::promise<Result<WireResponse>> promise;
    auto future = promise.get_future();
    std::thread client([&loop, endpoint, path, &promise] {
        promise.set_value(get_once(endpoint, path));
        loop->stop();
    });
    auto ran = loop->run();
    client.join();
    if (!ran) return Result<WireResponse>::failure(ran.error());
    auto response = future.get();
    auto stopped = service->stop();
    if (!stopped) return Result<WireResponse>::failure(stopped.error());
    service.reset();
    return response;
}

TEST(ServiceWebUiRouteTest, ApplicationsDisabledKeepsRootUnavailable) {
    auto response = run_get(ServiceOptions::defaults().value(), "/");
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status, 404);
}

TEST(ServiceWebUiRouteTest, ApplicationsEnabledServesWebUiAndExistingRoutes) {
    const auto root = std::filesystem::temp_directory_path() /
                      "iaisf-web-ui-service-route-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    auto options = options_with_applications(
        17, false, false, temporary_applications(root));
    ASSERT_TRUE(options);
    auto html = run_get(std::move(options).value(), "/");
    ASSERT_TRUE(html);
    EXPECT_EQ(html.value().status, 200);
    EXPECT_NE(html.value().body.find("<!doctype html>"), std::string::npos);

    auto css_options = options_with_applications(
        17, false, false, temporary_applications(root));
    ASSERT_TRUE(css_options);
    auto css = run_get(std::move(css_options).value(), "/ui/app.css");
    ASSERT_TRUE(css);
    EXPECT_EQ(css.value().status, 200);
    EXPECT_NE(css.value().body.find(".secondary"), std::string::npos);

    auto js_options = options_with_applications(
        17, false, false, temporary_applications(root));
    ASSERT_TRUE(js_options);
    auto javascript = run_get(std::move(js_options).value(), "/ui/app.js");
    ASSERT_TRUE(javascript);
    EXPECT_EQ(javascript.value().status, 200);
    EXPECT_NE(javascript.value().body.find("AbortController"), std::string::npos);

    auto viewer_options = options_with_applications(
        17, false, false, temporary_applications(root));
    ASSERT_TRUE(viewer_options);
    auto viewer = run_get(std::move(viewer_options).value(), "/ui/point-cloud-viewer.js");
    ASSERT_TRUE(viewer);
    EXPECT_EQ(viewer.value().status, 200);
    EXPECT_NE(viewer.value().body.find("IaisfPointCloudViewer"), std::string::npos);

    auto health = run_get(
        options_with_applications(17, false, false, temporary_applications(root)).value(),
        "/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health.value().status, 200);

    auto task = run_get(
        options_with_applications(17, false, false, temporary_applications(root)).value(),
        "/v1/tasks/unknown");
    ASSERT_TRUE(task);
    EXPECT_EQ(task.value().status, 400);

    auto application = run_get(
        options_with_applications(17, false, false, temporary_applications(root)).value(),
        "/api/weld-inspection/v1/jobs/wi_bad");
    ASSERT_TRUE(application);
    EXPECT_EQ(application.value().status, 404);

    auto artifact = run_get(
        options_with_applications(17, false, false, temporary_applications(root)).value(),
        "/api/artifacts/v1/files/pc_bad");
    ASSERT_TRUE(artifact);
    EXPECT_EQ(artifact.value().status, 404);
    std::filesystem::remove_all(root, cleanup_error);
}

}  // namespace
}  // namespace iaisf::service
