#include <gtest/gtest.h>

#include <string>

#include "iaisf/api/strict_json.hpp"

namespace iaisf::api {
namespace {

TEST(StrictJsonTest, ParsesValidObjectAfterPreflight) {
    const auto result = parse_strict_json(R"({"a":[1,true,"ok"]})");
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().at("a").size(), 3U);
}

TEST(StrictJsonTest, RejectsMalformedAndTrailingGarbage) {
    EXPECT_EQ(
        parse_strict_json(std::string{R"({"a":1})"} + "garbage")
            .error()
            .category,
        StrictJsonFailureCategory::MalformedJson);
}

TEST(StrictJsonTest, RejectsDuplicateKeysAtEveryObjectDepth) {
    const auto root = parse_strict_json(R"({"a":1,"a":2})");
    const auto nested = parse_strict_json(R"({"x":{"a":1,"a":2}})");
    ASSERT_FALSE(root);
    ASSERT_FALSE(nested);
    EXPECT_EQ(root.error().category, StrictJsonFailureCategory::DuplicateKey);
    EXPECT_EQ(nested.error().category, StrictJsonFailureCategory::DuplicateKey);
}

TEST(StrictJsonTest, RejectsConfiguredDepthAndNodeLimits) {
    StrictJsonLimits limits = StrictJsonLimits::defaults();
    limits.max_depth = 2U;
    EXPECT_EQ(
        parse_strict_json(R"({"a":{"b":1}})", limits).error().category,
        StrictJsonFailureCategory::DepthExceeded);
    limits.max_depth = 3U;
    EXPECT_TRUE(parse_strict_json(R"({"a":{"b":1}})", limits));
    limits.max_depth = 3U;
    EXPECT_EQ(
        parse_strict_json(R"({"a":{"b":{"c":1}}})", limits)
            .error()
            .category,
        StrictJsonFailureCategory::DepthExceeded);

    limits = StrictJsonLimits::defaults();
    limits.max_nodes = 3U;
    EXPECT_EQ(
        parse_strict_json(R"({"a":[1,2]})", limits).error().category,
        StrictJsonFailureCategory::NodeLimitExceeded);
    EXPECT_TRUE(parse_strict_json(R"({"a":[1]})", limits));
    limits.max_nodes = 3U;
    EXPECT_EQ(
        parse_strict_json(R"({"a":[1,2]})", limits).error().category,
        StrictJsonFailureCategory::NodeLimitExceeded);
}

TEST(StrictJsonTest, RejectsKeyAndStringByteLimits) {
    StrictJsonLimits limits = StrictJsonLimits::defaults();
    limits.max_key_bytes = 2U;
    EXPECT_EQ(
        parse_strict_json(R"({"long":1})", limits).error().category,
        StrictJsonFailureCategory::TextLimitExceeded);
    limits.max_key_bytes = 3U;
    EXPECT_TRUE(parse_strict_json(R"({"abc":1})", limits));

    limits = StrictJsonLimits::defaults();
    limits.max_string_bytes = 2U;
    EXPECT_EQ(
        parse_strict_json(R"({"a":"long"})", limits).error().category,
        StrictJsonFailureCategory::TextLimitExceeded);
    limits.max_string_bytes = 3U;
    EXPECT_TRUE(parse_strict_json(R"({"a":"xyz"})", limits));
}

TEST(StrictJsonTest, RejectsPayloadLimitInvalidUtf8CommentsAndNonFinite) {
    StrictJsonLimits limits = StrictJsonLimits::defaults();
    limits.max_bytes = 4U;
    EXPECT_EQ(
        parse_strict_json(R"({"a":1})", limits).error().category,
        StrictJsonFailureCategory::PayloadTooLarge);
    const std::string invalid_utf8{"{\"a\":\"\xFF\"}"};
    EXPECT_EQ(
        parse_strict_json(invalid_utf8).error().category,
        StrictJsonFailureCategory::MalformedJson);
    EXPECT_EQ(
        parse_strict_json(R"({"a":NaN})").error().category,
        StrictJsonFailureCategory::MalformedJson);
    EXPECT_EQ(
        parse_strict_json(R"({"a":1}//comment)").error().category,
        StrictJsonFailureCategory::MalformedJson);
    EXPECT_EQ(
        parse_strict_json(R"({"a":Infinity})").error().category,
        StrictJsonFailureCategory::MalformedJson);
    EXPECT_EQ(
        parse_strict_json(R"({"a":-Infinity})").error().category,
        StrictJsonFailureCategory::MalformedJson);

    limits = StrictJsonLimits::defaults();
    limits.max_bytes = 4U;
    EXPECT_TRUE(parse_strict_json("null", limits));
    EXPECT_EQ(
        parse_strict_json("null ", limits).error().category,
        StrictJsonFailureCategory::PayloadTooLarge);

    limits = StrictJsonLimits::defaults();
    limits.max_depth = 0U;
    EXPECT_EQ(
        parse_strict_json("null", limits).error().category,
        StrictJsonFailureCategory::InternalFailure);
    limits = StrictJsonLimits::defaults();
    limits.max_nodes = 0U;
    EXPECT_EQ(
        parse_strict_json("null", limits).error().category,
        StrictJsonFailureCategory::InternalFailure);
    limits = StrictJsonLimits::defaults();
    limits.max_bytes = 0U;
    EXPECT_EQ(
        parse_strict_json("null", limits).error().category,
        StrictJsonFailureCategory::InternalFailure);
    limits = StrictJsonLimits::defaults();
    limits.max_key_bytes = 0U;
    EXPECT_EQ(
        parse_strict_json("null", limits).error().category,
        StrictJsonFailureCategory::InternalFailure);
    limits = StrictJsonLimits::defaults();
    limits.max_string_bytes = 0U;
    EXPECT_EQ(
        parse_strict_json("null", limits).error().category,
        StrictJsonFailureCategory::InternalFailure);

    limits = StrictJsonLimits::defaults();
    limits.max_string_bytes = 4U;
    const auto error = parse_strict_json(
        std::string{"{\"payload\":\""} + std::string(80U, 'x') +
        "\"}",
        limits);
    ASSERT_FALSE(error);
    EXPECT_LT(error.error().message.size(), 128U);
    EXPECT_EQ(error.error().message.find("payload"), std::string::npos);
    EXPECT_EQ(error.error().message.find(std::string(32U, 'x')), std::string::npos);
}

}  // namespace
}  // namespace iaisf::api
