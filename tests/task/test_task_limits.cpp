#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/task/task_limits.hpp"

namespace {

using iaisf::ErrorCode;
using iaisf::task::TaskLimits;
using iaisf::task::TaskRequest;

TEST(TaskLimitsTest, ProvidesValidatedDefaults) {
    auto limits = TaskLimits::create();

    ASSERT_TRUE(limits);
    EXPECT_EQ(limits.value().max_repository_tasks(), 100000U);
    EXPECT_EQ(limits.value().max_operation_bytes(), 256U);
    EXPECT_EQ(limits.value().max_input_bytes(), 1024U * 1024U);
}

TEST(TaskLimitsTest, RejectsZeroNegativeAndHardLimitValues) {
    EXPECT_FALSE(TaskLimits::create(0));
    EXPECT_FALSE(TaskLimits::create(-1));
    EXPECT_FALSE(TaskLimits::create(1000001));
    EXPECT_FALSE(TaskLimits::create(1, 4097));
    EXPECT_FALSE(TaskLimits::create(1, 1, 67108865));
    EXPECT_FALSE(TaskLimits::create(1, 1, 1, 67108865));
    EXPECT_FALSE(TaskLimits::create(1, 1, 1, 1, 65537));
}

TEST(TaskLimitsTest, RejectsEmptyWhitespaceAndControlOperations) {
    const auto limits = TaskLimits::create().value();

    EXPECT_FALSE(limits.validate_request(TaskRequest{"", {}}));
    EXPECT_FALSE(limits.validate_request(TaskRequest{"   ", {}}));
    EXPECT_FALSE(limits.validate_request(TaskRequest{"bad\nname", {}}));
}

TEST(TaskLimitsTest, EnforcesOperationByteLimit) {
    const auto limits = TaskLimits::create(4, 4, 32, 32, 16).value();

    EXPECT_TRUE(limits.validate_request(TaskRequest{"four", {}}));
    auto oversized = limits.validate_request(TaskRequest{"five!", {}});
    EXPECT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, ErrorCode::ResourceExhausted);
}

TEST(TaskLimitsTest, EnforcesSerializedInputAndResultByteLimits) {
    const auto limits = TaskLimits::create(4, 32, 8, 8, 16).value();
    const nlohmann::json small = "abc";
    const nlohmann::json large = "0123456789";

    EXPECT_TRUE(limits.validate_request(TaskRequest{"op", small}));
    EXPECT_FALSE(limits.validate_request(TaskRequest{"op", large}));
    EXPECT_TRUE(limits.validate_result(small));
    EXPECT_FALSE(limits.validate_result(large));
}

TEST(TaskLimitsTest, RejectsJsonThatCannotSerializeAsUtf8) {
    const auto limits = TaskLimits::create().value();
    const std::string invalid_utf8(1, static_cast<char>(0xFF));

    auto validated =
        limits.validate_request(TaskRequest{"op", nlohmann::json{invalid_utf8}});

    EXPECT_FALSE(validated);
    EXPECT_EQ(validated.error().code, ErrorCode::InvalidArgument);
    auto result_validated =
        limits.validate_result(nlohmann::json{invalid_utf8});
    EXPECT_FALSE(result_validated);
    EXPECT_EQ(result_validated.error().code, ErrorCode::InvalidArgument);
}

TEST(TaskLimitsTest, AcceptsEveryJsonValueKindAndCountsUtf8Bytes) {
    const auto broad_limits = TaskLimits::create(8, 32, 64, 64, 16).value();
    EXPECT_TRUE(broad_limits.validate_request(TaskRequest{"null", nullptr}));
    EXPECT_TRUE(broad_limits.validate_request(
        TaskRequest{"array", nlohmann::json::array({1, 2})}));
    EXPECT_TRUE(broad_limits.validate_request(TaskRequest{"string", "value"}));
    EXPECT_TRUE(broad_limits.validate_request(TaskRequest{"number", 42}));
    EXPECT_TRUE(broad_limits.validate_request(TaskRequest{"boolean", true}));

    const std::string utf8_character{"\xC3\xA9"};
    const nlohmann::json utf8_string = utf8_character;
    const auto exact = TaskLimits::create(2, 32, 4, 4, 16).value();
    const auto too_small = TaskLimits::create(2, 32, 3, 3, 16).value();
    EXPECT_TRUE(exact.validate_request(
        TaskRequest{"utf8", utf8_string}));
    EXPECT_TRUE(exact.validate_result(utf8_string));
    EXPECT_FALSE(too_small.validate_request(
        TaskRequest{"utf8", utf8_string}));
    EXPECT_FALSE(too_small.validate_result(utf8_string));
}

TEST(TaskLimitsTest, BoundsExternalErrorMessagesWithoutLeakingPrefix) {
    const auto limits = TaskLimits::create(4, 32, 32, 32, 4).value();
    const iaisf::Error original{ErrorCode::InternalError, "secret detail"};

    auto sanitized = limits.sanitize_error(original);

    ASSERT_TRUE(sanitized);
    EXPECT_EQ(sanitized.value().message, "####");
    EXPECT_EQ(original.message, "secret detail");
}

}  // namespace
