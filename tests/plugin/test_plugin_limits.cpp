#include <cstdint>

#include <gtest/gtest.h>

#include "iaisf/plugin/plugin_limits.hpp"

namespace {

using iaisf::ErrorCode;
using iaisf::plugin::PluginLimits;

TEST(PluginLimitsTest, DefaultsArePositiveValidatedSafetyBounds) {
    const auto limits = PluginLimits::create();

    ASSERT_TRUE(limits);
    EXPECT_GT(limits.value().max_plugins(), 0U);
    EXPECT_GT(limits.value().max_operation_bytes(), 0U);
    EXPECT_GT(limits.value().max_name_bytes(), 0U);
    EXPECT_GT(limits.value().max_version_bytes(), 0U);
    EXPECT_GT(limits.value().max_description_bytes(), 0U);
    EXPECT_GT(limits.value().max_error_message_bytes(), 0U);
    EXPECT_GT(limits.value().max_input_bytes(), 0U);
    EXPECT_GT(limits.value().max_output_bytes(), 0U);
    EXPECT_GT(limits.value().max_json_depth(), 0U);
    EXPECT_GT(limits.value().max_json_elements(), 0U);
    EXPECT_GT(limits.value().max_string_bytes(), 0U);
    EXPECT_GT(limits.value().max_capabilities(), 0U);
    EXPECT_GT(limits.value().max_capability_bytes(), 0U);
}

TEST(PluginLimitsTest, PreservesExplicitValues) {
    const auto limits =
        PluginLimits::create(3, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22);

    ASSERT_TRUE(limits);
    EXPECT_EQ(limits.value().max_plugins(), 3U);
    EXPECT_EQ(limits.value().max_operation_bytes(), 11U);
    EXPECT_EQ(limits.value().max_name_bytes(), 12U);
    EXPECT_EQ(limits.value().max_version_bytes(), 13U);
    EXPECT_EQ(limits.value().max_description_bytes(), 14U);
    EXPECT_EQ(limits.value().max_error_message_bytes(), 15U);
    EXPECT_EQ(limits.value().max_input_bytes(), 16U);
    EXPECT_EQ(limits.value().max_output_bytes(), 17U);
    EXPECT_EQ(limits.value().max_json_depth(), 18U);
    EXPECT_EQ(limits.value().max_json_elements(), 19U);
    EXPECT_EQ(limits.value().max_string_bytes(), 20U);
    EXPECT_EQ(limits.value().max_capabilities(), 21U);
    EXPECT_EQ(limits.value().max_capability_bytes(), 22U);
}

TEST(PluginLimitsTest, RejectsZeroAndNegativeBeforeUnsignedConversion) {
    const std::int64_t valid = 1;
    const std::int64_t invalid_values[] = {0, -1};
    for (const auto invalid : invalid_values) {
        const auto plugins =
            PluginLimits::create(invalid, valid, valid, valid, valid, valid);
        const auto operation =
            PluginLimits::create(valid, invalid, valid, valid, valid, valid);
        const auto name =
            PluginLimits::create(valid, valid, invalid, valid, valid, valid);
        const auto version =
            PluginLimits::create(valid, valid, valid, invalid, valid, valid);
        const auto description =
            PluginLimits::create(valid, valid, valid, valid, invalid, valid);
        const auto error =
            PluginLimits::create(valid, valid, valid, valid, valid, invalid);

        EXPECT_EQ(plugins.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(operation.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(name.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(version.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(description.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(error.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(PluginLimitsTest, RejectsValuesAboveHardMaximumWithoutClamping) {
    EXPECT_EQ(
        PluginLimits::create(100001).error().code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 4097).error().code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 1, 4097).error().code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 1, 1, 1025).error().code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 1, 1, 1, 65537).error().code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 1, 1, 1, 1, 65537).error().code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 67108865).error().code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 1, 67108865)
            .error()
            .code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 1, 1, 257)
            .error()
            .code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 1, 1, 1, 1000001)
            .error()
            .code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 16777217)
            .error()
            .code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1025)
            .error()
            .code,
        ErrorCode::InvalidArgument);
    EXPECT_EQ(
        PluginLimits::create(
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 4097)
            .error()
            .code,
        ErrorCode::InvalidArgument);
}

TEST(PluginLimitsTest, RejectsZeroForEveryJsonAndCapabilityLimit) {
    EXPECT_FALSE(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 0));
    EXPECT_FALSE(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 1, 0));
    EXPECT_FALSE(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 1, 1, 0));
    EXPECT_FALSE(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 1, 1, 1, 0));
    EXPECT_FALSE(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0));
    EXPECT_FALSE(
        PluginLimits::create(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0));
    EXPECT_FALSE(
        PluginLimits::create(
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0));
}

TEST(PluginLimitsTest, AcceptsDocumentedHardMaximumsExactly) {
    const auto limits =
        PluginLimits::create(
            100000,
            4096,
            4096,
            1024,
            65536,
            65536,
            67108864,
            67108864,
            256,
            1000000,
            16777216,
            1024,
            4096);

    ASSERT_TRUE(limits);
    EXPECT_EQ(limits.value().max_plugins(), 100000U);
    EXPECT_EQ(limits.value().max_description_bytes(), 65536U);
    EXPECT_EQ(limits.value().max_output_bytes(), 67108864U);
    EXPECT_EQ(limits.value().max_json_elements(), 1000000U);
}

}  // namespace
