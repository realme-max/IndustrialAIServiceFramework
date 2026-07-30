#include <cstdint>

#include <gtest/gtest.h>

#include "iaisf/core/error.hpp"
#include "iaisf/http/http_limits.hpp"

namespace iaisf::http {
namespace {

TEST(HttpLimitsTest, DefaultsArePositiveAndLogicallyConsistent) {
    const auto limits = HttpLimits::defaults();

    EXPECT_GT(limits.max_request_line_bytes(), 0U);
    EXPECT_GE(
        limits.max_request_line_bytes(),
        limits.max_method_bytes() + limits.max_target_bytes() + 12U);
    EXPECT_LE(limits.max_header_line_bytes(), limits.max_header_bytes());
    EXPECT_GT(limits.max_requests_per_dispatch(), 0U);
}

TEST(HttpLimitsTest, SignedFactoryPreservesValidValues) {
    auto result = HttpLimits::create(
        256, 16, 128, 64, 256, 12, 1024, 2048, 32, 4);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().max_request_line_bytes(), 256U);
    EXPECT_EQ(result.value().max_header_count(), 12U);
    EXPECT_EQ(result.value().max_response_body_bytes(), 2048U);
}

TEST(HttpLimitsTest, RejectsNegativeBeforeUnsignedConversion) {
    auto result = HttpLimits::create(
        256, -1, 128, 64, 256, 12, 1024, 2048, 32, 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpLimitsTest, RejectsZeroCount) {
    auto result = HttpLimits::create(
        256, 16, 128, 64, 256, 0, 1024, 2048, 32, 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpLimitsTest, RejectsRequestLineRelationship) {
    auto result = HttpLimits::create(
        100, 32, 64, 64, 256, 12, 1024, 2048, 32, 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpLimitsTest, RejectsHeaderLineLargerThanTotal) {
    auto result = HttpLimits::create(
        256, 16, 128, 257, 256, 12, 1024, 2048, 32, 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpLimitsTest, RejectsHeaderLineThatCannotContainCrlf) {
    auto result = HttpLimits::create(
        256, 16, 128, 1, 256, 12, 1024, 2048, 32, 4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(HttpLimitsTest, RejectsValuesAboveHardBounds) {
    auto result = HttpLimits::create(
        HttpLimits::kMaximumByteLimit + 1,
        16,
        128,
        64,
        256,
        12,
        1024,
        2048,
        32,
        4);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
}  // namespace iaisf::http
