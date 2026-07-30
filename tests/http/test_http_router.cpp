#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "iaisf/core/error.hpp"
#include "iaisf/http/builtin_routes.hpp"
#include "iaisf/http/http_request.hpp"
#include "iaisf/http/http_router.hpp"

namespace iaisf::http {
namespace {

HttpRequest request(std::string method, std::string target) {
    auto result = HttpRequest::create(
        std::move(method),
        std::move(target),
        {{"host", "localhost"}},
        "",
        true);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

Result<HttpResponse> ok_handler(const HttpRequest&) {
    HttpResponse response;
    response.set_body("ok");
    return Result<HttpResponse>::success(std::move(response));
}

TEST(HttpRouterTest, ExactMethodAndPathDispatchAfterFreeze) {
    HttpRouter router;
    ASSERT_TRUE(router.add_route("GET", "/x", ok_handler));
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("GET", "/x?query=1"));

    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::Ok);
    EXPECT_EQ(response.value().body(), "ok");
}

TEST(HttpRouterTest, DispatchBeforeFreezeFails) {
    HttpRouter router;
    ASSERT_TRUE(router.add_route("GET", "/x", ok_handler));

    auto response = router.dispatch(request("GET", "/x"));

    ASSERT_FALSE(response);
    EXPECT_EQ(response.error().code, ErrorCode::InvalidState);
}

TEST(HttpRouterTest, DuplicateRouteIsRejectedWithoutReplacement) {
    HttpRouter router;
    ASSERT_TRUE(router.add_route("GET", "/x", ok_handler));

    auto duplicate = router.add_route("GET", "/x", ok_handler);

    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(router.route_count(), 1U);
}

TEST(HttpRouterTest, RegistrationAfterFreezeIsRejected) {
    HttpRouter router;
    ASSERT_TRUE(router.freeze());

    auto result = router.add_route("GET", "/x", ok_handler);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
}

TEST(HttpRouterTest, UnknownPathReturns404WithoutClosingKeepAlive) {
    HttpRouter router;
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("GET", "/missing"));

    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::NotFound);
    EXPECT_FALSE(response.value().close_connection());
}

TEST(HttpRouterTest, MethodMismatchReturns405AndSortedAllow) {
    HttpRouter router;
    ASSERT_TRUE(router.add_route("POST", "/x", ok_handler));
    ASSERT_TRUE(router.add_route("GET", "/x", ok_handler));
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("DELETE", "/x"));

    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::MethodNotAllowed);
    ASSERT_TRUE(response.value().headers().size() >= 2U);
    auto serialized = response.value().serialize(HttpLimits::defaults());
    ASSERT_TRUE(serialized);
    EXPECT_NE(serialized.value().find("Allow: GET, POST\r\n"), std::string::npos);
}

TEST(HttpRouterTest, HandlerFailureBecomesClosed500) {
    HttpRouter router;
    ASSERT_TRUE(router.add_route(
        "GET",
        "/x",
        [](const HttpRequest&) {
            return Result<HttpResponse>::failure(make_error(
                ErrorCode::InternalError,
                "secret internal detail"));
        }));
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("GET", "/x"));

    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::InternalServerError);
    EXPECT_TRUE(response.value().close_connection());
    EXPECT_EQ(
        response.value().body().find("secret"),
        std::string::npos);
}

TEST(HttpRouterTest, HandlerExceptionBecomesClosed500) {
    HttpRouter router;
    ASSERT_TRUE(router.add_route(
        "GET",
        "/x",
        [](const HttpRequest&) -> Result<HttpResponse> {
            throw std::runtime_error{"secret exception"};
        }));
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("GET", "/x"));

    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::InternalServerError);
    EXPECT_TRUE(response.value().close_connection());
}

TEST(HttpRouterTest, UnknownHandlerExceptionBecomes500AndRouterStillWorks) {
    HttpRouter router;
    ASSERT_TRUE(router.add_route(
        "GET",
        "/unknown",
        [](const HttpRequest&) -> Result<HttpResponse> {
            throw 7;
        }));
    ASSERT_TRUE(router.add_route("GET", "/ok", ok_handler));
    ASSERT_TRUE(router.freeze());

    auto failed = router.dispatch(request("GET", "/unknown"));
    auto healthy = router.dispatch(request("GET", "/ok"));

    ASSERT_TRUE(failed);
    EXPECT_EQ(failed.value().status(), HttpStatus::InternalServerError);
    ASSERT_TRUE(healthy);
    EXPECT_EQ(healthy.value().status(), HttpStatus::Ok);
}

TEST(HttpRouterTest, HandlerHeaderLimitFailureBecomesSafeClosed500) {
    auto limits =
        HttpLimits::create(256, 16, 128, 64, 256, 3, 1024, 1024, 8, 2);
    ASSERT_TRUE(limits);
    HttpRouter router{limits.value()};
    ASSERT_TRUE(router.add_route(
        "GET",
        "/headers",
        [](const HttpRequest&) {
            HttpResponse response;
            auto first = response.set_header("X-First", "1");
            auto second = response.set_header("X-Second", "2");
            if (!first) {
                return Result<HttpResponse>::failure(
                    std::move(first).error());
            }
            if (!second) {
                return Result<HttpResponse>::failure(
                    std::move(second).error());
            }
            return Result<HttpResponse>::success(std::move(response));
        }));
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("GET", "/headers"));

    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::InternalServerError);
    EXPECT_TRUE(response.value().close_connection());
    EXPECT_EQ(response.value().body(), "Internal Server Error\n");
    EXPECT_TRUE(response.value().serialize(limits.value()));
}

TEST(HttpRouterTest, PathComparisonIsCaseSensitive) {
    HttpRouter router;
    ASSERT_TRUE(router.add_route("GET", "/Case", ok_handler));
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("GET", "/case"));

    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::NotFound);
}

TEST(HttpRouterTest, RouteCapacityIsHardBound) {
    auto limits =
        HttpLimits::create(128, 16, 64, 64, 256, 8, 64, 64, 1, 2);
    ASSERT_TRUE(limits);
    HttpRouter router{std::move(limits).value()};
    ASSERT_TRUE(router.add_route("GET", "/one", ok_handler));

    auto result = router.add_route("GET", "/two", ok_handler);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
}

TEST(HttpRouterTest, RejectsEmptyHandlerAndNonExactPathSyntax) {
    HttpRouter router;

    EXPECT_FALSE(router.add_route("GET", "/x", {}));
    EXPECT_FALSE(router.add_route("GET", "x", ok_handler));
    EXPECT_FALSE(router.add_route("GET", "/x?query", ok_handler));
}

TEST(HttpRouterTest, BuiltinHealthReturnsStableJson) {
    HttpRouter router;
    ASSERT_TRUE(register_builtin_routes(router));
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("GET", "/health"));

    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), HttpStatus::Ok);
    EXPECT_EQ(response.value().body(), "{\"status\":\"ok\"}");
}

TEST(HttpRouterTest, BuiltinVersionUsesGeneratedProjectVersion) {
    HttpRouter router;
    ASSERT_TRUE(register_builtin_routes(router));
    ASSERT_TRUE(router.freeze());

    auto response = router.dispatch(request("GET", "/version"));

    ASSERT_TRUE(response);
    EXPECT_EQ(
        response.value().body(),
        "{\"name\":\"IndustrialAIServiceFramework\",\"version\":\"0.1.0\"}");
}

}  // namespace
}  // namespace iaisf::http
