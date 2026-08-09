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

TEST(WebUiHttpApiTest, RegistersFourResourcesWithSecureHeaders) {
    auto api = WebUiHttpApi::create(http::HttpLimits::defaults());
    ASSERT_TRUE(api);

    http::HttpRouter router;
    ASSERT_TRUE(api.value()->register_routes(router));
    EXPECT_EQ(router.route_count(), 4U);
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
    EXPECT_NE(response.value().body().find("/ui/point-cloud-viewer.js"), std::string::npos);
    EXPECT_LT(response.value().body().find("/ui/point-cloud-viewer.js"),
              response.value().body().find("/ui/app.js"));
    EXPECT_EQ(response.value().body().find("<script>"), std::string::npos);
    EXPECT_EQ(response.value().body().find("style="), std::string::npos);
    EXPECT_EQ(response.value().body().find("http://"), std::string::npos);
    EXPECT_EQ(response.value().body().find("https://"), std::string::npos);
    EXPECT_EQ(response.value().body().find("visualization-placeholder"), std::string::npos);
    EXPECT_EQ(response.value().body().find("Phase 10C"), std::string::npos);
    EXPECT_NE(response.value().body().find("viewer-description"), std::string::npos);
    EXPECT_NE(response.value().body().find(
                  "PTV2 &#x4E0E; WeldAgent &#x4E3A;&#x4E24;&#x4E2A;&#x72EC;&#x7ACB;&#x4E1A;&#x52A1;&#x3002;"),
              std::string::npos);
    EXPECT_EQ(response.value().body().find(
                  "&#x8D28;&#x91CF;&#x8BC4;&#x4EF7;&#x5C1A;&#x672A;&#x5B9E;&#x73B0;"),
              std::string::npos);
    EXPECT_EQ(response.value().body().find(
                  "&#x673A;&#x5668;&#x4EBA;&#x6267;&#x884C;&#x59CB;&#x7EC8;&#x5173;&#x95ED;"),
              std::string::npos);
    EXPECT_EQ(response.value().body().find("class=\"notice\""),
              std::string::npos);
    EXPECT_EQ(response.value().body().find(
                  "&#x63D0;&#x4EA4;&#x540E;&#x9700;&#x8981;&#x4EBA;&#x5DE5;&#x590D;&#x6838;"),
              std::string::npos);
    EXPECT_NE(response.value().body().find(
                  "&#x8F93;&#x51FA;&#x5750;&#x6807;&#x8F74;&#x3001;&#x8D77;&#x70B9;&#x3001;&#x7EC8;&#x70B9;&#x53CA;&#x53EF;&#x9009;&#x62D0;&#x70B9;"),
              std::string::npos);
    EXPECT_NE(response.value().body().find("<option value=\"l\">l</option>"),
              std::string::npos);
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
    const auto viewer = router.dispatch(
        make_request("GET", "/ui/point-cloud-viewer.js"));
    ASSERT_TRUE(css);
    ASSERT_TRUE(javascript);
    ASSERT_TRUE(viewer);
    EXPECT_EQ(header_value(css.value(), "Content-Type"),
              "text/css; charset=utf-8");
    EXPECT_EQ(header_value(javascript.value(), "Content-Type"),
              "application/javascript; charset=utf-8");
    EXPECT_EQ(header_value(viewer.value(), "Content-Type"),
              "application/javascript; charset=utf-8");
    EXPECT_NE(css.value().body().find(".panel"), std::string::npos);
    EXPECT_NE(javascript.value().body().find("AbortController"),
              std::string::npos);
    EXPECT_NE(javascript.value().body().find("textContent"),
              std::string::npos);
    EXPECT_EQ(javascript.value().body().find("innerHTML"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("IaisfPointCloudViewer"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("webgl2"), std::string::npos);
    EXPECT_EQ(viewer.value().body().find("eval("), std::string::npos);
    EXPECT_EQ(viewer.value().body().find("innerHTML"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("canonicalDownload"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("500000"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("xyz-f32le"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("[1,0,0,1]"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("[0,1,0,1]"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("[0,0,1,1]"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("pointercancel"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("lostpointercapture"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("context lost"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("canonicalDownload"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("DECIMAL_INTEGER"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("DECIMAL_FLOAT"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("parseUnsignedInteger"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("parseFiniteDecimal"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("Number.isSafeInteger(value)"), std::string::npos);
    EXPECT_NE(viewer.value().body().find("values[3] !== \"0\""), std::string::npos);
    EXPECT_NE(viewer.value().body().find("MAX_PATH_SAMPLES_PER_SEGMENT"),
              std::string::npos);
    EXPECT_NE(viewer.value().body().find("MAX_AXIS_SAMPLES"),
              std::string::npos);
    EXPECT_NE(viewer.value().body().find("MAX_GUIDANCE_AUX_POINTS"),
              std::string::npos);
    EXPECT_NE(viewer.value().body().find("sampleSegments"),
              std::string::npos);
    EXPECT_NE(viewer.value().body().find("gl.POINTS, 14"),
              std::string::npos);
    EXPECT_NE(viewer.value().body().find("gl.POINTS, 9"),
              std::string::npos);
    EXPECT_NE(viewer.value().body().find("gl.POINTS, 6"),
              std::string::npos);
    EXPECT_NE(viewer.value().body().find("const axisLength = .22"),
              std::string::npos);
    EXPECT_NE(viewer.value().body().find("pathPoints.push(result.corner)"),
              std::string::npos);
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
    EXPECT_NE(body.find(
                  "result.quality_assessment !== \"not_implemented\""),
              std::string::npos);
    EXPECT_NE(body.find("result.robot_execution_allowed !== false"),
              std::string::npos);
    EXPECT_NE(body.find("human_checkpoint: \"not_required\""),
              std::string::npos);
    EXPECT_EQ(body.find("human_checkpoint: \"required\""),
              std::string::npos);
    EXPECT_NE(body.find("[\"\\u72b6\\u6001\", result.disposition]"),
              std::string::npos);
    EXPECT_EQ(body.find("[\"\\u53ef\\u4fe1\\u5ea6\", result.confidence]"),
              std::string::npos);
    EXPECT_EQ(body.find(
                  "[\"\\u4eba\\u5de5\\u590d\\u6838\\u539f\\u56e0\", result.waiting_reason]"),
              std::string::npos);
    EXPECT_EQ(body.find(
                  "[\"\\u673a\\u5668\\u4eba\\u6267\\u884c\", String(result.robot_execution_allowed)]"),
              std::string::npos);
    EXPECT_NE(body.find(
                  "[\"\\u957f\\u5ea6\", withUnit(result.length_mm, \"mm\")]"),
              std::string::npos);
    EXPECT_NE(body.find(
                  "[\"\\u63a8\\u7406\\u8017\\u65f6\", withUnit(result.inference_time_ms, \"ms\")]"),
              std::string::npos);
    EXPECT_NE(body.find(
                  "[\"\\u603b\\u8017\\u65f6\", withUnit(result.total_time_ms, \"ms\")]"),
              std::string::npos);
    EXPECT_EQ(body.find(
                  "[\"\\u8d28\\u91cf\\u8bc4\\u4ef7\", result.quality_assessment]"),
              std::string::npos);
    EXPECT_EQ(body.find("innerHTML"), std::string::npos);
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
