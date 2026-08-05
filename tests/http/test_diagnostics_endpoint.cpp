#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

#include "iaisf/http/diagnostics_routes.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/mock_vision_plugin.hpp"
#include "iaisf/plugin/plugin_runtime.hpp"
#include "iaisf/task/task_manager.hpp"

namespace iaisf::http {

namespace {
class QuietLogger final : public ILogger {
public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};
}

TEST(DiagnosticsEndpointTest, RunningDiagnosticsReturnsJsonWithoutStore) {
    QuietLogger logger;
    MetricsRegistry metrics;
    auto health = std::make_shared<health::HealthChecker>();
    auto manager_result = task::TaskManager::create(
        task::ThreadPoolOptions{1U, 4U}, task::TaskLimits::create().value(),
        logger,
        [](const task::TaskRequest&) {
            return Result<nlohmann::json>::success(nlohmann::json::object());
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    auto diagnostics_result = diagnostics::RuntimeDiagnostics::create(
        health, *manager, metrics);
    ASSERT_TRUE(diagnostics_result);

    HttpRouter router;
    ASSERT_TRUE(register_diagnostics_route(
        router, diagnostics_result.value(), "/debug/status", 4096U));
    ASSERT_TRUE(router.freeze());
    auto request = HttpRequest::create("GET", "/debug/status", {}, {}, false);
    ASSERT_TRUE(request);
    auto response = router.dispatch(request.value());
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::Ok);
    EXPECT_NE(response.value().body().find("schema_version"), std::string::npos);
    bool has_cache = false;
    for (const auto& header : response.value().headers()) {
        has_cache = has_cache ||
            (header.name == "Cache-Control" && header.value == "no-store");
    }
    EXPECT_TRUE(has_cache);
    ASSERT_NE(health->transition_to(health::HealthPhase::Stopping),
              health::HealthTransitionOutcome::InvalidTransition);
    ASSERT_NE(health->transition_to(health::HealthPhase::Stopped),
              health::HealthTransitionOutcome::InvalidTransition);
    auto stopped_response = router.dispatch(request.value());
    ASSERT_TRUE(stopped_response);
    EXPECT_EQ(stopped_response.value().status(), HttpStatus::ServiceUnavailable);
    ASSERT_TRUE(manager->shutdown());
}

TEST(DiagnosticsEndpointTest, ExpiredDiagnosticsReturnsUnavailable) {
    HttpRouter router;
    auto registered = register_diagnostics_route(
        router, std::weak_ptr<const diagnostics::RuntimeDiagnostics>{},
        "/debug/status", 4096U);
    ASSERT_TRUE(registered);
    ASSERT_TRUE(router.freeze());

    auto request = HttpRequest::create(
        "GET", "/debug/status", {}, {}, false);
    ASSERT_TRUE(request);
    auto response = router.dispatch(request.value());
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::ServiceUnavailable);
    ASSERT_NE(response.value().body().find("unavailable"), std::string::npos);
    bool has_cache = false;
    for (const auto& header : response.value().headers()) {
        if (header.name == "Cache-Control" && header.value == "no-store") {
            has_cache = true;
        }
    }
    EXPECT_TRUE(has_cache);
}

TEST(DiagnosticsEndpointTest, MethodMismatchIsRejected) {
    HttpRouter router;
    ASSERT_TRUE(register_diagnostics_route(
        router, std::weak_ptr<const diagnostics::RuntimeDiagnostics>{},
        "/debug/status", 4096U));
    ASSERT_TRUE(router.freeze());
    auto request = HttpRequest::create("POST", "/debug/status", {}, {}, false);
    ASSERT_TRUE(request);
    auto response = router.dispatch(request.value());
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::MethodNotAllowed);
}

TEST(DiagnosticsEndpointTest, DisabledEndpointHasNoRoute) {
    HttpRouter router;
    ASSERT_TRUE(router.freeze());
    auto request = HttpRequest::create("GET", "/debug/status", {}, {}, false);
    ASSERT_TRUE(request);
    auto response = router.dispatch(request.value());
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::NotFound);
}

TEST(DiagnosticsEndpointTest, ResponseLimitFailsClosedWith503) {
    QuietLogger logger;
    MetricsRegistry metrics;
    auto health = std::make_shared<health::HealthChecker>();
    auto manager_result = task::TaskManager::create(
        task::ThreadPoolOptions{1U, 4U}, task::TaskLimits::create().value(),
        logger,
        [](const task::TaskRequest&) {
            return Result<nlohmann::json>::success(nlohmann::json::object());
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    auto diagnostics_result = diagnostics::RuntimeDiagnostics::create(
        health, *manager, metrics);
    ASSERT_TRUE(diagnostics_result);
    HttpRouter router;
    ASSERT_TRUE(register_diagnostics_route(
        router, diagnostics_result.value(), "/debug/status", 1U));
    ASSERT_TRUE(router.freeze());
    auto request = HttpRequest::create("GET", "/debug/status", {}, {}, false);
    ASSERT_TRUE(request);
    auto response = router.dispatch(request.value());
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::ServiceUnavailable);
    ASSERT_TRUE(manager->shutdown());
}

TEST(DiagnosticsEndpointTest, IncludesStablePluginDiagnostics) {
    QuietLogger logger;
    MetricsRegistry metrics;
    auto health = std::make_shared<health::HealthChecker>();
    auto manager_result = task::TaskManager::create(
        task::ThreadPoolOptions{1U, 4U}, task::TaskLimits::create().value(),
        logger,
        [](const task::TaskRequest&) {
            return Result<nlohmann::json>::success(nlohmann::json::object());
        },
        &metrics);
    ASSERT_TRUE(manager_result);
    auto manager = std::move(manager_result).value();
    auto runtime_result = plugin::PluginRuntime::create(
        plugin::PluginLimits::create().value(), &metrics);
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<plugin::MockVisionPlugin>()));
    ASSERT_TRUE(runtime->register_plugin(
        std::make_shared<plugin::EchoPlugin>()));
    ASSERT_TRUE(runtime->freeze());
    auto diagnostics_result = diagnostics::RuntimeDiagnostics::create(
        health, *manager, metrics, nullptr,
        std::weak_ptr<const plugin::PluginRuntime>{runtime});
    ASSERT_TRUE(diagnostics_result);

    HttpRouter router;
    ASSERT_TRUE(register_diagnostics_route(
        router, diagnostics_result.value(), "/debug/status", 8192U));
    ASSERT_TRUE(router.freeze());
    auto request = HttpRequest::create("GET", "/debug/status", {}, {}, false);
    ASSERT_TRUE(request);
    auto response = router.dispatch(request.value());
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::Ok);
    const auto& body = response.value().body();
    const auto plugins_pos = body.find("\"plugins\"");
    const auto entries_pos = body.find("\"entries\"");
    const auto echo_pos = body.find("echo", entries_pos);
    const auto mock_pos = body.find("mock_vision.detect", entries_pos);
    EXPECT_NE(plugins_pos, std::string::npos);
    EXPECT_NE(body.find("\"available\":true", plugins_pos),
              std::string::npos);
    EXPECT_LT(entries_pos, body.size());
    EXPECT_LT(echo_pos, mock_pos);
    auto post = HttpRequest::create("POST", "/debug/status", {}, {}, false);
    ASSERT_TRUE(post);
    auto method_response = router.dispatch(post.value());
    ASSERT_TRUE(method_response);
    EXPECT_EQ(method_response.value().status(), HttpStatus::MethodNotAllowed);

    ASSERT_TRUE(runtime->shutdown());
    ASSERT_TRUE(manager->shutdown());
}

} // namespace iaisf::http
