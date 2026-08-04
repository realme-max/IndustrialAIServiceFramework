#include <cstddef>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_parser.hpp"
#include "iaisf/http/http_status.hpp"

namespace iaisf::http {
namespace {

constexpr std::string_view kGet{
    "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n"};

ParseProgress parse_once(HttpParser& parser, std::string_view bytes) {
    auto result = parser.parse(bytes);
    EXPECT_TRUE(result);
    return result.value();
}

void expect_protocol_error(
    std::string_view request,
    HttpStatus expected,
    HttpLimits limits = HttpLimits::defaults()) {
    HttpParser parser{std::move(limits)};
    const auto progress = parse_once(parser, request);
    EXPECT_EQ(progress.disposition, ParseDisposition::Error);
    EXPECT_EQ(progress.error_status, expected);
}

TEST(HttpParserTest, ParsesCompleteGetAndNormalizesHeaders) {
    HttpParser parser;

    const auto progress = parse_once(
        parser,
        "GET /health?full=1 HTTP/1.1\r\n"
        "hOsT:\t localhost \t\r\nX-Test: value\r\n\r\n");

    EXPECT_EQ(progress.disposition, ParseDisposition::Complete);
    auto request = parser.take_request();
    ASSERT_TRUE(request);
    EXPECT_EQ(request.value().method(), "GET");
    EXPECT_EQ(request.value().path(), "/health");
    EXPECT_EQ(request.value().query(), "full=1");
    EXPECT_EQ(*request.value().header("host"), "localhost");
    EXPECT_TRUE(request.value().keep_alive());
}

TEST(HttpParserTest, ParsesEveryByteAsSeparateFragment) {
    HttpParser parser;
    for (std::size_t index = 0U; index < kGet.size(); ++index) {
        const auto progress = parse_once(parser, kGet.substr(index, 1U));
        EXPECT_EQ(progress.consumed, 1U);
        if (index + 1U == kGet.size()) {
            EXPECT_EQ(progress.disposition, ParseDisposition::Complete);
        } else {
            EXPECT_EQ(progress.disposition, ParseDisposition::NeedMore);
        }
    }
    ASSERT_TRUE(parser.take_request());
}

TEST(HttpParserTest, ParsesPostWithBinaryBodyAcrossFragments) {
    HttpParser parser;
    const std::string head{
        "POST /data HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 3\r\n\r\n"};
    const auto headers = parse_once(parser, head);
    EXPECT_EQ(headers.disposition, ParseDisposition::NeedMore);
    EXPECT_EQ(headers.phase, ParsePhase::Body);
    EXPECT_EQ(headers.body_bytes_consumed, 0U);
    const auto first_body =
        parse_once(parser, std::string_view{"a\0", 2U});
    EXPECT_EQ(first_body.disposition, ParseDisposition::NeedMore);
    EXPECT_EQ(first_body.body_bytes_consumed, 2U);
    const auto final_body = parse_once(parser, "b");
    EXPECT_EQ(final_body.disposition, ParseDisposition::Complete);
    EXPECT_EQ(final_body.body_bytes_consumed, 1U);
    EXPECT_EQ(parser.phase(), ParsePhase::Complete);

    auto request = parser.take_request();
    ASSERT_TRUE(request);
    EXPECT_EQ(request.value().body(), std::string("a\0b", 3U));
}

TEST(HttpParserTest, ReportsOnlyBodyBytesConsumedFromMixedInput) {
    HttpParser parser;
    const auto progress = parse_once(
        parser,
        "POST /data HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 4\r\n\r\nab");

    EXPECT_EQ(progress.disposition, ParseDisposition::NeedMore);
    EXPECT_EQ(progress.phase, ParsePhase::Body);
    EXPECT_EQ(progress.body_bytes_consumed, 2U);
}

TEST(HttpParserTest, AcceptsExplicitZeroContentLength) {
    HttpParser parser;
    const auto progress = parse_once(
        parser,
        "POST /data HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 0\r\n\r\n");

    ASSERT_EQ(progress.disposition, ParseDisposition::Complete);
    auto request = parser.take_request();
    ASSERT_TRUE(request);
    EXPECT_TRUE(request.value().body().empty());
}

TEST(HttpParserTest, ReportsOnlyConsumedFirstRequestInPipeline) {
    HttpParser parser;
    const std::string bytes{std::string{kGet} + std::string{kGet}};

    const auto first = parse_once(parser, bytes);

    ASSERT_EQ(first.disposition, ParseDisposition::Complete);
    EXPECT_EQ(first.consumed, kGet.size());
    ASSERT_TRUE(parser.take_request());
    const auto second = parse_once(
        parser,
        std::string_view{bytes}.substr(first.consumed));
    EXPECT_EQ(second.disposition, ParseDisposition::Complete);
}

TEST(HttpParserTest, IncompleteInputNeedsMoreWithoutError) {
    HttpParser parser;

    const auto progress = parse_once(parser, "GET / HTTP/1.1\r\nHost:");

    EXPECT_EQ(progress.disposition, ParseDisposition::NeedMore);
    EXPECT_EQ(parser.disposition(), ParseDisposition::NeedMore);
}

TEST(HttpParserTest, PreservesMethodCaseForCaseSensitiveRouting) {
    HttpParser parser;
    ASSERT_EQ(
        parse_once(
            parser,
            "get / HTTP/1.1\r\nHost: localhost\r\n\r\n")
            .disposition,
        ParseDisposition::Complete);

    auto request = parser.take_request();
    ASSERT_TRUE(request);
    EXPECT_EQ(request.value().method(), "get");
}

TEST(HttpParserTest, ConnectionCloseDisablesKeepAlive) {
    HttpParser parser;
    ASSERT_EQ(
        parse_once(
            parser,
            "GET / HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: keep-alive, CLOSE\r\n\r\n")
            .disposition,
        ParseDisposition::Complete);

    auto request = parser.take_request();
    ASSERT_TRUE(request);
    EXPECT_FALSE(request.value().keep_alive());
}

TEST(HttpParserTest, ConnectionTokenMatchingIsExactAndCaseInsensitive) {
    HttpParser parser;
    ASSERT_EQ(
        parse_once(
            parser,
            "GET / HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: xclose, close-x\r\n\r\n")
            .disposition,
        ParseDisposition::Complete);
    auto non_close = parser.take_request();
    ASSERT_TRUE(non_close);
    EXPECT_TRUE(non_close.value().keep_alive());

    ASSERT_EQ(
        parse_once(
            parser,
            "GET / HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: Keep-Alive, CLOSE\r\n\r\n")
            .disposition,
        ParseDisposition::Complete);
    auto close = parser.take_request();
    ASSERT_TRUE(close);
    EXPECT_FALSE(close.value().keep_alive());
}

TEST(HttpParserTest, RejectsEmptyConnectionToken) {
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close,\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: localhost\r\n"
        "Connection:\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: keep-alive,,close\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: clo(se\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsBareLf) {
    expect_protocol_error(
        "GET / HTTP/1.1\nHost: localhost\n\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsCarriageReturnNotFollowedByLf) {
    expect_protocol_error(
        "GET / HTTP/1.1\rX",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsMalformedRequestLine) {
    expect_protocol_error(
        "GET  / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsInvalidMethodToken) {
    expect_protocol_error(
        "GE\tT / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsHttp10) {
    expect_protocol_error(
        "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n",
        HttpStatus::HttpVersionNotSupported);
}

TEST(HttpParserTest, RejectsHttp2Preface) {
    expect_protocol_error(
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n",
        HttpStatus::HttpVersionNotSupported);
}

TEST(HttpParserTest, RejectsAbsoluteAuthorityAndAsteriskTargets) {
    expect_protocol_error(
        "GET http://example.test/ HTTP/1.1\r\nHost: example.test\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "CONNECT example.test:443 HTTP/1.1\r\nHost: example.test\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "OPTIONS * HTTP/1.1\r\nHost: example.test\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsTargetFragmentAndControlBytes) {
    expect_protocol_error(
        "GET /path#fragment HTTP/1.1\r\nHost: x\r\n\r\n",
        HttpStatus::BadRequest);
    constexpr char kTargetWithNul[] =
        "GET /pa\0th HTTP/1.1\r\nHost: x\r\n\r\n";
    expect_protocol_error(
        std::string_view{kTargetWithNul, sizeof(kTargetWithNul) - 1U},
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RequiresExactlyOneNonemptyHost) {
    expect_protocol_error(
        "GET / HTTP/1.1\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: \r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsDuplicateContentLengthEvenWhenEqual) {
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
        "Content-Length: 1\r\n\r\nx",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsCaseVariantDuplicateContentLength) {
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
        "cOnTeNt-LeNgTh: 1\r\n\r\nx",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsEveryDuplicateNormalizedHeaderName) {
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "X-Test: one\r\nX-Test: two\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "X-Test: one\r\nx-tEsT: two\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Connection: keep-alive\r\nconnection: close\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\nhOsT: y\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsInvalidAndOverflowingContentLength) {
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 999999999999999999999999\r\n\r\n",
        HttpStatus::PayloadTooLarge);
}

TEST(HttpParserTest, TrimsContentLengthOwsButRejectsAmbiguousForms) {
    HttpParser parser;
    ASSERT_EQ(
        parse_once(
            parser,
            "POST / HTTP/1.1\r\nHost: x\r\n"
            "Content-Length:\t 1 \t\r\n\r\nx")
            .disposition,
        ParseDisposition::Complete);
    ASSERT_TRUE(parser.take_request());

    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: +1\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1,1\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1.0\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, ContentLengthPlusTransferEncodingIsBadRequest) {
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n"
        "Transfer-Encoding: chunked\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, AnyTransferEncodingIsNotImplemented) {
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: chunked\r\n\r\n",
        HttpStatus::NotImplemented);
}

TEST(HttpParserTest, ExpectIsExpectationFailed) {
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 1\r\n\r\n",
        HttpStatus::ExpectationFailed);
}

TEST(HttpParserTest, AppliesStableConflictingErrorPriority) {
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n"
        "Expect: 100-continue\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Transfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n",
        HttpStatus::NotImplemented);
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\n"
        "Expect: 100-continue\r\nContent-Length: 999999\r\n\r\n",
        HttpStatus::ExpectationFailed);
    expect_protocol_error(
        "POST / HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, UpgradeIsNotImplemented) {
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\n"
        "Connection: upgrade\r\nUpgrade: websocket\r\n\r\n",
        HttpStatus::NotImplemented);
}

TEST(HttpParserTest, RejectsObsFoldAndInvalidHeaderName) {
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\nBad Name: x\r\n\r\n",
        HttpStatus::BadRequest);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost : x\r\n\r\n",
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, RejectsNulAndControlInHeaderValue) {
    constexpr char kHeaderWithNul[] =
        "GET / HTTP/1.1\r\nHost: x\r\nX-Test: a\0b\r\n\r\n";
    expect_protocol_error(
        std::string_view{kHeaderWithNul, sizeof(kHeaderWithNul) - 1U},
        HttpStatus::BadRequest);
    constexpr char kHeaderWithControl[] =
        "GET / HTTP/1.1\r\nHost: x\r\nX-Test: a"
        "\x01"
        "b\r\n\r\n";
    expect_protocol_error(
        std::string_view{
            kHeaderWithControl,
            sizeof(kHeaderWithControl) - 1U},
        HttpStatus::BadRequest);
}

TEST(HttpParserTest, MapsTargetLimitTo414) {
    auto limits =
        HttpLimits::create(128, 16, 16, 64, 256, 8, 64, 64, 8, 2);
    ASSERT_TRUE(limits);

    expect_protocol_error(
        "GET /1234567890123456 HTTP/1.1\r\nHost: x\r\n\r\n",
        HttpStatus::UriTooLong,
        std::move(limits).value());
}

TEST(HttpParserTest, MapsOverlongMethodAndRequestLineFailClosed) {
    auto method_limits =
        HttpLimits::create(128, 4, 64, 64, 256, 8, 64, 64, 8, 2);
    ASSERT_TRUE(method_limits);
    expect_protocol_error(
        "GETTT / HTTP/1.1\r\nHost: x\r\n\r\n",
        HttpStatus::BadRequest,
        std::move(method_limits).value());

    auto line_limits =
        HttpLimits::create(92, 16, 64, 64, 256, 8, 64, 64, 8, 2);
    ASSERT_TRUE(line_limits);
    const std::string overlong{
        std::string(17U, 'M') + " /" + std::string(63U, 'x') +
        " HTTP/1.1\r\nHost: x\r\n\r\n"};
    expect_protocol_error(
        overlong,
        HttpStatus::UriTooLong,
        std::move(line_limits).value());
}

TEST(HttpParserTest, MapsHeaderLineAndTotalLimitsTo431) {
    auto line_limits =
        HttpLimits::create(128, 16, 64, 16, 64, 8, 64, 64, 8, 2);
    ASSERT_TRUE(line_limits);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: xxxxxxxxxxxxxxxxx\r\n\r\n",
        HttpStatus::RequestHeaderFieldsTooLarge,
        std::move(line_limits).value());

    auto total_limits =
        HttpLimits::create(128, 16, 64, 16, 20, 8, 64, 64, 8, 2);
    ASSERT_TRUE(total_limits);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\nX-One: abc\r\n\r\n",
        HttpStatus::RequestHeaderFieldsTooLarge,
        std::move(total_limits).value());
}

TEST(HttpParserTest, MapsHeaderCountLimitTo431) {
    auto limits =
        HttpLimits::create(128, 16, 64, 64, 256, 1, 64, 64, 8, 2);
    ASSERT_TRUE(limits);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: x\r\nX-Test: y\r\n\r\n",
        HttpStatus::RequestHeaderFieldsTooLarge,
        std::move(limits).value());
}

TEST(HttpParserTest, MapsBodyLimitTo413BeforeAllocatingBody) {
    auto limits =
        HttpLimits::create(128, 16, 64, 64, 256, 8, 2, 64, 8, 2);
    ASSERT_TRUE(limits);
    expect_protocol_error(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\n",
        HttpStatus::PayloadTooLarge,
        std::move(limits).value());
}

TEST(HttpParserTest, TakeRequestRequiresCompleteRequest) {
    HttpParser parser;

    auto request = parser.take_request();

    ASSERT_FALSE(request);
    EXPECT_EQ(request.error().code, ErrorCode::InvalidState);
}

TEST(HttpParserTest, ErrorStateIsTerminalAndConsumesNoLaterBytes) {
    HttpParser parser;
    const auto first = parse_once(parser, "GET / HTTP/1.0\r\n");
    ASSERT_EQ(first.disposition, ParseDisposition::Error);

    const auto second = parse_once(parser, kGet);

    EXPECT_EQ(second.disposition, ParseDisposition::Error);
    EXPECT_EQ(second.consumed, 0U);
    EXPECT_EQ(
        second.error_status,
        HttpStatus::HttpVersionNotSupported);
}

TEST(HttpParserTest, UnterminatedLinesStopScanningAtConfiguredLimits) {
    auto request_limits =
        HttpLimits::create(92, 16, 64, 32, 64, 8, 64, 64, 8, 2);
    ASSERT_TRUE(request_limits);
    expect_protocol_error(
        std::string(93U, 'A'),
        HttpStatus::UriTooLong,
        request_limits.value());

    HttpParser parser{std::move(request_limits).value()};
    ASSERT_EQ(
        parse_once(parser, "GET / HTTP/1.1\r\n").disposition,
        ParseDisposition::NeedMore);
    const auto header = parse_once(parser, std::string(33U, 'H'));
    EXPECT_EQ(header.disposition, ParseDisposition::Error);
    EXPECT_EQ(
        header.error_status,
        HttpStatus::RequestHeaderFieldsTooLarge);
}

TEST(HttpParserTest, LineAndHeaderTotalsIncludeCrlfExactly) {
    auto exact =
        HttpLimits::create(16, 3, 1, 9, 11, 1, 64, 64, 8, 2);
    ASSERT_TRUE(exact);
    HttpParser parser{exact.value()};
    const auto progress = parse_once(
        parser,
        "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_EQ(progress.disposition, ParseDisposition::Complete);
    EXPECT_TRUE(parser.take_request());

    auto line_exceeded =
        HttpLimits::create(16, 3, 1, 9, 32, 1, 64, 64, 8, 2);
    ASSERT_TRUE(line_exceeded);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: xx\r\n\r\n",
        HttpStatus::RequestHeaderFieldsTooLarge,
        line_exceeded.value());

    auto total_exceeded =
        HttpLimits::create(16, 3, 1, 11, 11, 1, 64, 64, 8, 2);
    ASSERT_TRUE(total_exceeded);
    expect_protocol_error(
        "GET / HTTP/1.1\r\nHost: xx\r\n\r\n",
        HttpStatus::RequestHeaderFieldsTooLarge,
        total_exceeded.value());
}

}  // namespace
}  // namespace iaisf::http
