#include <string>

#include <gtest/gtest.h>

#include "iaisf/core/error.hpp"
#include "iaisf/http/http_request.hpp"
#include "iaisf/http/http_response.hpp"
#include "iaisf/http/http_status.hpp"

namespace iaisf::http {
namespace {

HttpRequest make_request(
    std::string method = "GET",
    std::string target = "/health?verbose=1",
    bool keep_alive = true) {
    auto result = HttpRequest::create(
        std::move(method),
        std::move(target),
        {{"host", "localhost"}, {"x-test", "value"}},
        std::string{"a\0b", 3U},
        keep_alive);
    EXPECT_TRUE(result);
    return std::move(result).value();
}

TEST(HttpRequestTest, OwnsRawTargetPathQueryHeadersAndBinaryBody) {
    const auto request = make_request();

    EXPECT_EQ(request.method(), "GET");
    EXPECT_EQ(request.target(), "/health?verbose=1");
    EXPECT_EQ(request.path(), "/health");
    EXPECT_EQ(request.query(), "verbose=1");
    EXPECT_EQ(request.body(), std::string("a\0b", 3U));
    EXPECT_TRUE(request.keep_alive());
    ASSERT_TRUE(request.header("host").has_value());
    EXPECT_EQ(*request.header("host"), "localhost");
}

TEST(HttpRequestTest, HeaderLookupReturnsOwningCopy) {
    const auto request = make_request();
    auto value = request.header("x-test");
    ASSERT_TRUE(value.has_value());
    value->assign("changed");

    EXPECT_EQ(*request.header("x-test"), "value");
}

TEST(HttpRequestTest, RejectsNonOriginTarget) {
    auto result = HttpRequest::create(
        "GET", "http://example.test/", {}, "", true);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpRequestTest, RejectsHeaderNameThatIsNotLowercaseToken) {
    auto result = HttpRequest::create(
        "GET", "/", {{"Host", "localhost"}}, "", true);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpRequestTest, RejectsDuplicateNormalizedHeaders) {
    auto result = HttpRequest::create(
        "GET",
        "/",
        {{"host", "first"}, {"host", "second"}},
        "",
        true);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpResponseTest, SerializesStableHttp11FramingAndBinaryBody) {
    HttpResponse response{HttpStatus::Ok};
    ASSERT_TRUE(response.set_header("Content-Type", "application/octet-stream"));
    response.set_body(std::string{"x\0y", 3U});

    auto serialized = response.serialize(HttpLimits::defaults());

    ASSERT_TRUE(serialized);
    EXPECT_NE(serialized.value().find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(
        serialized.value().find("Content-Length: 3\r\n"),
        std::string::npos);
    EXPECT_NE(
        serialized.value().find("Connection: keep-alive\r\n"),
        std::string::npos);
    EXPECT_EQ(
        serialized.value().substr(serialized.value().size() - 3U),
        std::string("x\0y", 3U));
}

TEST(HttpResponseTest, EmitsConnectionCloseWhenRequested) {
    HttpResponse response;
    response.set_close_connection(true);

    auto serialized = response.serialize(HttpLimits::defaults());

    ASSERT_TRUE(serialized);
    EXPECT_NE(
        serialized.value().find("Connection: close\r\n"),
        std::string::npos);
}

TEST(HttpResponseTest, RejectsResponseSplitting) {
    HttpResponse response;

    auto result = response.set_header("X-Test", "safe\r\nInjected: yes");
    auto name_result = response.set_header("X-Test\r\nInjected", "safe");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    ASSERT_FALSE(name_result);
    EXPECT_EQ(name_result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpResponseTest, RejectsFrameworkOwnedFramingHeadersCaseInsensitively) {
    HttpResponse response;

    EXPECT_FALSE(response.set_header("content-length", "1"));
    EXPECT_FALSE(response.set_header("Connection", "close"));
    EXPECT_FALSE(response.set_header("TRANSFER-ENCODING", "chunked"));
}

TEST(HttpResponseTest, ReplacesExistingCustomHeaderCaseInsensitively) {
    HttpResponse response;
    ASSERT_TRUE(response.set_header("X-Test", "one"));
    ASSERT_TRUE(response.set_header("x-test", "two"));

    ASSERT_EQ(response.headers().size(), 1U);
    EXPECT_EQ(response.headers().front().value, "two");
}

TEST(HttpResponseTest, RejectsBodyAboveConfiguredLimit) {
    auto limits_result =
        HttpLimits::create(256, 16, 128, 64, 256, 12, 1024, 2, 32, 4);
    ASSERT_TRUE(limits_result);
    HttpResponse response;
    response.set_body("abc");

    auto serialized = response.serialize(limits_result.value());

    ASSERT_FALSE(serialized);
    EXPECT_EQ(serialized.error().code, ErrorCode::ResourceExhausted);
}

TEST(HttpResponseTest, RejectsHeadersAboveConfiguredLimit) {
    auto limits_result =
        HttpLimits::create(256, 16, 128, 32, 64, 12, 1024, 1024, 32, 4);
    ASSERT_TRUE(limits_result);
    HttpResponse response;
    ASSERT_TRUE(response.set_header("X-Large", std::string(80U, 'x')));

    auto serialized = response.serialize(limits_result.value());

    ASSERT_FALSE(serialized);
    EXPECT_EQ(serialized.error().code, ErrorCode::ResourceExhausted);
}

TEST(HttpResponseTest, EnforcesHeaderCountIncludingAutomaticHeaders) {
    auto exact_limits =
        HttpLimits::create(256, 16, 128, 64, 256, 3, 1024, 1024, 32, 4);
    auto exceeded_limits =
        HttpLimits::create(256, 16, 128, 64, 256, 2, 1024, 1024, 32, 4);
    ASSERT_TRUE(exact_limits);
    ASSERT_TRUE(exceeded_limits);
    HttpResponse response;
    ASSERT_TRUE(response.set_header("X-Test", "value"));

    EXPECT_TRUE(response.serialize(exact_limits.value()));
    auto exceeded = response.serialize(exceeded_limits.value());
    ASSERT_FALSE(exceeded);
    EXPECT_EQ(exceeded.error().code, ErrorCode::ResourceExhausted);
}

TEST(HttpResponseTest, EnforcesExactHeaderLineLimitIncludingFramingBytes) {
    auto limits =
        HttpLimits::create(256, 16, 128, 24, 256, 4, 1024, 1024, 32, 4);
    ASSERT_TRUE(limits);
    HttpResponse exact;
    ASSERT_TRUE(exact.set_header("X", std::string(19U, 'x')));
    EXPECT_TRUE(exact.serialize(limits.value()));

    HttpResponse exceeded;
    ASSERT_TRUE(exceeded.set_header("X", std::string(20U, 'x')));
    auto serialized = exceeded.serialize(limits.value());
    ASSERT_FALSE(serialized);
    EXPECT_EQ(serialized.error().code, ErrorCode::ResourceExhausted);
}

TEST(HttpResponseTest, EnforcesHeaderTotalIncludingStatusAndAutomaticHeaders) {
    auto exact_limits =
        HttpLimits::create(256, 16, 128, 24, 62, 2, 1024, 1024, 32, 4);
    auto exceeded_limits =
        HttpLimits::create(256, 16, 128, 24, 61, 2, 1024, 1024, 32, 4);
    ASSERT_TRUE(exact_limits);
    ASSERT_TRUE(exceeded_limits);
    HttpResponse response;

    auto exact = response.serialize(exact_limits.value());
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact.value().size(), 62U);
    auto exceeded = response.serialize(exceeded_limits.value());
    ASSERT_FALSE(exceeded);
    EXPECT_EQ(exceeded.error().code, ErrorCode::ResourceExhausted);
}

TEST(HttpResponseTest, FailedPreflightProducesNoPartialSerializedValue) {
    auto limits =
        HttpLimits::create(256, 16, 128, 24, 61, 2, 1024, 1024, 32, 4);
    ASSERT_TRUE(limits);
    HttpResponse response;

    auto serialized = response.serialize(limits.value());

    ASSERT_FALSE(serialized);
    EXPECT_EQ(serialized.error().code, ErrorCode::ResourceExhausted);
}

TEST(HttpResponseTest, PreservesStableCustomHeaderInsertionOrder) {
    HttpResponse response;
    ASSERT_TRUE(response.set_header("X-Second", "2"));
    ASSERT_TRUE(response.set_header("X-First", "1"));

    auto serialized = response.serialize(HttpLimits::defaults());

    ASSERT_TRUE(serialized);
    const auto second = serialized.value().find("X-Second: 2\r\n");
    const auto first = serialized.value().find("X-First: 1\r\n");
    ASSERT_NE(second, std::string::npos);
    ASSERT_NE(first, std::string::npos);
    EXPECT_LT(second, first);
}

TEST(HttpResponseTest, ErrorResponseIsPlainTextAndClosedByDefault) {
    auto response = HttpResponse::error(HttpStatus::BadRequest);

    EXPECT_EQ(response.status(), HttpStatus::BadRequest);
    EXPECT_TRUE(response.close_connection());
    EXPECT_EQ(response.body(), "Bad Request\n");
    ASSERT_FALSE(response.headers().empty());
    EXPECT_EQ(
        response.headers().front().value,
        "text/plain; charset=utf-8");
}

TEST(HttpStatusTest, ProvidesStableReasonPhrases) {
    EXPECT_EQ(reason_phrase(HttpStatus::Ok), "OK");
    EXPECT_EQ(reason_phrase(HttpStatus::Accepted), "Accepted");
    EXPECT_EQ(reason_phrase(HttpStatus::RequestTimeout), "Request Timeout");
    EXPECT_EQ(
        reason_phrase(HttpStatus::UnsupportedMediaType),
        "Unsupported Media Type");
    EXPECT_EQ(
        reason_phrase(HttpStatus::UnprocessableContent),
        "Unprocessable Content");
    EXPECT_EQ(
        reason_phrase(HttpStatus::HttpVersionNotSupported),
        "HTTP Version Not Supported");
    EXPECT_EQ(
        reason_phrase(HttpStatus::ServiceUnavailable),
        "Service Unavailable");
}

TEST(HttpResponseTest, SerializesEveryRequiredErrorStatusWithoutPartialFailure) {
    const HttpStatus statuses[] = {
        HttpStatus::BadRequest,
        HttpStatus::NotFound,
        HttpStatus::MethodNotAllowed,
        HttpStatus::RequestTimeout,
        HttpStatus::PayloadTooLarge,
        HttpStatus::UriTooLong,
        HttpStatus::ExpectationFailed,
        HttpStatus::RequestHeaderFieldsTooLarge,
        HttpStatus::InternalServerError,
        HttpStatus::NotImplemented,
        HttpStatus::ServiceUnavailable,
        HttpStatus::HttpVersionNotSupported,
    };
    for (const auto status : statuses) {
        auto response = HttpResponse::error(status);
        auto serialized = response.serialize(HttpLimits::defaults());
        ASSERT_TRUE(serialized);
        EXPECT_NE(
            serialized.value().find(
                std::to_string(static_cast<int>(status))),
            std::string::npos);
    }
}

TEST(HttpResponseTest, SerializesClosedErrorAtExactHeaderLineLimit) {
    auto constrained_limits =
        HttpLimits::create(256, 32, 128, 41, 256, 8, 4, 1024, 16, 4);
    ASSERT_TRUE(constrained_limits);

    const auto response = HttpResponse::error(HttpStatus::BadRequest);
    const auto serialized =
        response.serialize(constrained_limits.value());

    ASSERT_TRUE(serialized);
    EXPECT_NE(
        serialized.value().find(
            "Content-Type: text/plain; charset=utf-8\r\n"),
        std::string::npos);
    EXPECT_NE(
        serialized.value().find("Content-Length: "),
        std::string::npos);
    EXPECT_NE(
        serialized.value().find("Connection: close\r\n"),
        std::string::npos);
    EXPECT_EQ(
        serialized.value().substr(
            serialized.value().size() - response.body().size()),
        response.body());
}

}  // namespace
}  // namespace iaisf::http
