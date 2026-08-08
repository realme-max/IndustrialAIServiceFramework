#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "iaisf/http/http_request.hpp"
#include "iaisf/http/http_router.hpp"
#include "iaisf/http/web_ui_http_api.hpp"

namespace iaisf::web_ui {
namespace {

http::HttpRequest make_request(
    std::string method,
    std::string target) {
    auto request = http::HttpRequest::create(
        std::move(method),
        std::move(target),
        { {"host", "localhost"} },
        "",
        true);
    EXPECT_TRUE(request);
    return std::move(request).value();
}

std::string header_value(
    const http::HttpResponse& response,
    const std::string& name) {
    for (const auto& header : response.headers()) {
        if (header.name == name) {
            return header.value;
        }
    }
    return {};
}

TEST(WebUiHttpApiTest, RegistersThreeResourcesWithSecureHeaders) {
    auto api = WebUiHttpApi::create(http::HttpLimits::defaults());
    ASSERT_TRUE(api);

    http::HttpRouter router;
    ASSERT_TRUE(api.value()->register_routes(router));
    EXPECT_EQ(router.route_count(), 3U);
    ASSERT_TRUE(router.freeze());

    const auto response = router.dispatch(make_request("GET", "/"));
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), http::HttpStatus::Ok);
    EXPECT_EQ(header_value(response.value(), "Content-Type"),
              "text/html; charset=utf-8");
    EXPECT_EQ(header_value(response.value(), "Cache-Control"), "no-store");
    EXPECT_EQ(header_value(response.value(), "X-Content-Type-Options"),
              "nosniff");
    EXPECT_EQ(header_value(response.value(), "Referrer-Policy"),
              "no-referrer");
    EXPECT_EQ(header_value(response.value(), "X-Frame-Options"), "DENY");
    const auto csp = header_value(
        response.value(), "Content-Security-Policy");
    EXPECT_NE(csp.find("default-src 'none'"), std::string::npos);
    EXPECT_NE(csp.find("script-src 'self'"), std::string::npos);
    EXPECT_NE(csp.find("style-src 'self'"), std::string::npos);
    EXPECT_NE(csp.find("frame-ancestors 'none'"), std::string::npos);
    EXPECT_NE(response.value().body().find("/ui/app.css"), std::string::npos);
    EXPECT_NE(response.value().body().find("/ui/app.js"), std::string::npos);
    EXPECT_EQ(response.value().body().find("<script>"), std::string::npos);
    EXPECT_EQ(response.value().body().find("style="), std::string::npos);
    EXPECT_EQ(response.value().body().find("http://"), std::string::npos);
    EXPECT_EQ(response.value().body().find("https://"), std::string::npos);
}

TEST(WebUiHttpApiTest, ResourcesHaveExpectedMediaTypesAndAccessibleContent) {
    auto api = WebUiHttpApi::create(http::HttpLimits::defaults());
    ASSERT_TRUE(api);
    http::HttpRouter router;
    ASSERT_TRUE(api.value()->register_routes(router));
    ASSERT_TRUE(router.freeze());

    const auto css = router.dispatch(make_request("GET", "/ui/app.css"));
    const auto javascript = router.dispatch(
        make_request("GET", "/ui/app.js"));
    ASSERT_TRUE(css);
    ASSERT_TRUE(javascript);
    EXPECT_EQ(header_value(css.value(), "Content-Type"),
              "text/css; charset=utf-8");
    EXPECT_EQ(header_value(javascript.value(), "Content-Type"),
              "application/javascript; charset=utf-8");
    EXPECT_NE(css.value().body().find(".panel"), std::string::npos);
    EXPECT_NE(javascript.value().body().find("AbortController"),
              std::string::npos);
    EXPECT_NE(javascript.value().body().find("textContent"),
              std::string::npos);
    EXPECT_EQ(javascript.value().body().find("innerHTML"), std::string::npos);
}

TEST(WebUiHttpApiTest, ClientContractValidationAndAbortControlsAreEmbedded) {
    auto api = WebUiHttpApi::create(http::HttpLimits::defaults());
    ASSERT_TRUE(api);
    http::HttpRouter router;
    ASSERT_TRUE(api.value()->register_routes(router));
    ASSERT_TRUE(router.freeze());
    const auto javascript = router.dispatch(
        make_request("GET", "/ui/app.js"));
    ASSERT_TRUE(javascript);
    const auto& body = javascript.value().body();
    EXPECT_NE(body.find("body.error"), std::string::npos);
    EXPECT_NE(body.find("genericErrors"), std::string::npos);
    EXPECT_NE(body.find("TypeError"), std::string::npos);
    EXPECT_NE(body.find("if (!response.ok) throw httpError(response.status, body);"),
              std::string::npos);
    for (const auto status : {"400:", "409:", "413:", "415:", "422:", "500:", "503:"}) {
        EXPECT_NE(body.find(status), std::string::npos);
    }
    EXPECT_NE(body.find("operation.inputPointCount = artifact.point_count"), std::string::npos);
    EXPECT_NE(body.find("inputPointCount"), std::string::npos);
    EXPECT_NE(body.find("inspection-stop"), std::string::npos);
    EXPECT_NE(body.find("\\u5df2\\u505c\\u6b62\\u9875\\u9762\\u7b49\\u5f85"), std::string::npos);
    EXPECT_NE(body.find("removeEventListener"), std::string::npos);
    EXPECT_NE(body.find("signal.aborted"), std::string::npos);
    EXPECT_NE(body.find("const pollingStates"), std::string::npos);
    EXPECT_NE(body.find("!pollingStates.has(phase)"), std::string::npos);
    EXPECT_NE(body.find("const finalPhase = await poll"), std::string::npos);
    EXPECT_NE(body.find("isCurrent(operation) && finalPhase !== null"), std::string::npos);
    EXPECT_NE(body.find("const stopped = stopOperation()"), std::string::npos);
    EXPECT_NE(body.find("canonicalDownloadId"), std::string::npos);
    EXPECT_NE(body.find("quality_assessment"), std::string::npos);
    EXPECT_NE(body.find("robot_execution_allowed"), std::string::npos);
    EXPECT_NE(body.find("waiting_human"), std::string::npos);
    EXPECT_NE(body.find("setResultText"), std::string::npos);
}

TEST(WebUiHttpApiTest, MethodMismatchAndUnknownPathUseRouterSemantics) {
    auto api = WebUiHttpApi::create(http::HttpLimits::defaults());
    ASSERT_TRUE(api);
    http::HttpRouter router;
    ASSERT_TRUE(api.value()->register_routes(router));
    ASSERT_TRUE(router.freeze());

    const auto method = router.dispatch(make_request("POST", "/"));
    const auto unknown = router.dispatch(make_request("GET", "/ui/missing"));
    ASSERT_TRUE(method);
    ASSERT_TRUE(unknown);
    EXPECT_EQ(method.value().status(), http::HttpStatus::MethodNotAllowed);
    EXPECT_EQ(header_value(method.value(), "Allow"), "GET");
    EXPECT_EQ(unknown.value().status(), http::HttpStatus::NotFound);
}

TEST(WebUiHttpApiTest, CreationFailsClosedWhenResponseLimitCannotFitResources) {
    auto limits = http::HttpLimits::create(
        16 * 1024, 32, 8 * 1024, 8 * 1024, 32 * 1024, 100,
        1024 * 1024, 64, 256, 16);
    ASSERT_TRUE(limits);
    auto api = WebUiHttpApi::create(std::move(limits).value());
    EXPECT_FALSE(api);
    EXPECT_EQ(api.error().code, ErrorCode::ResourceExhausted);
}

TEST(WebUiHttpApiTest, ExpiredOwnerFailsClosed) {
    auto api = WebUiHttpApi::create(http::HttpLimits::defaults());
    ASSERT_TRUE(api);
    http::HttpRouter router;
    ASSERT_TRUE(api.value()->register_routes(router));
    api.value().reset();
    ASSERT_TRUE(router.freeze());

    const auto response = router.dispatch(make_request("GET", "/"));
    ASSERT_TRUE(response);
    EXPECT_EQ(response.value().status(), http::HttpStatus::InternalServerError);
    EXPECT_TRUE(response.value().close_connection());
}

TEST(WebUiHttpApiTest, RegistrationAfterFreezeIsRejected) {
    auto api = WebUiHttpApi::create(http::HttpLimits::defaults());
    ASSERT_TRUE(api);
    http::HttpRouter router;
    ASSERT_TRUE(router.freeze());
    auto registered = api.value()->register_routes(router);
    EXPECT_FALSE(registered);
    EXPECT_EQ(registered.error().code, ErrorCode::InvalidState);
}

}  // namespace
}  // namespace iaisf::web_ui
