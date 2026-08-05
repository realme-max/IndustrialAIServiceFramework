#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "iaisf/health/health_checker.hpp"
#include "iaisf/http/health_routes.hpp"
#include "iaisf/http/http_request.hpp"
#include "iaisf/http/http_router.hpp"

namespace {

iaisf::http::HttpRequest request(const std::string& path) {
    auto result = iaisf::http::HttpRequest::create(
        "GET", path, {{"host", "localhost"}}, {}, true);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

struct HealthRouter {
    std::shared_ptr<iaisf::health::HealthChecker> checker =
        std::make_shared<iaisf::health::HealthChecker>();
    iaisf::http::HttpRouter router;

    HealthRouter() {
        EXPECT_TRUE(iaisf::http::register_health_routes(router, checker));
        EXPECT_TRUE(router.freeze());
    }
};

TEST(HealthEndpointTest, HealthIsLiveBeforeAndDuringShutdown) {
    HealthRouter fixture;
    auto response = fixture.router.dispatch(request("/health"));
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), iaisf::http::HttpStatus::Ok);
    ASSERT_FALSE(response.value().headers().empty());
    EXPECT_EQ(response.value().headers().front().value,
              "application/json; charset=utf-8");
    EXPECT_NE(response.value().body().find("\"phase\":\"created\""),
              std::string::npos);

    ASSERT_EQ(
        fixture.checker->transition_to(iaisf::health::HealthPhase::Running),
        iaisf::health::HealthTransitionOutcome::Applied);
    ASSERT_EQ(
        fixture.checker->transition_to(iaisf::health::HealthPhase::Stopping),
        iaisf::health::HealthTransitionOutcome::Applied);
    response = fixture.router.dispatch(request("/health"));
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), iaisf::http::HttpStatus::Ok);
    EXPECT_NE(response.value().body().find("\"ready\":false"),
              std::string::npos);
}

TEST(HealthEndpointTest, ReadinessRequiresRunning) {
    HealthRouter fixture;
    auto response = fixture.router.dispatch(request("/ready"));
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), iaisf::http::HttpStatus::ServiceUnavailable);
    ASSERT_FALSE(response.value().headers().empty());
    EXPECT_EQ(response.value().headers().front().value,
              "application/json; charset=utf-8");

    ASSERT_EQ(
        fixture.checker->transition_to(iaisf::health::HealthPhase::Running),
        iaisf::health::HealthTransitionOutcome::Applied);
    response = fixture.router.dispatch(request("/ready"));
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), iaisf::http::HttpStatus::Ok);
    EXPECT_NE(response.value().body().find("\"status\":\"ready\""),
              std::string::npos);
}

TEST(HealthEndpointTest, StoppedAndExpiredCheckerFailClosed) {
    HealthRouter fixture;
    ASSERT_EQ(
        fixture.checker->transition_to(iaisf::health::HealthPhase::Stopping),
        iaisf::health::HealthTransitionOutcome::Applied);
    ASSERT_EQ(
        fixture.checker->transition_to(iaisf::health::HealthPhase::Stopped),
        iaisf::health::HealthTransitionOutcome::Applied);
    auto response = fixture.router.dispatch(request("/health"));
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), iaisf::http::HttpStatus::ServiceUnavailable);
    fixture.checker.reset();
    response = fixture.router.dispatch(request("/ready"));
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), iaisf::http::HttpStatus::ServiceUnavailable);
    EXPECT_TRUE(response.value().close_connection());
}

}  // namespace
