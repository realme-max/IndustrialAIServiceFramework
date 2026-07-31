#include <string>

#include <gtest/gtest.h>

#include "iaisf/plugin/plugin_metadata.hpp"

namespace {

using iaisf::ErrorCode;
using iaisf::plugin::PluginLimits;
using iaisf::plugin::PluginMetadata;
using iaisf::plugin::validate_metadata;

PluginMetadata valid_metadata() {
    return PluginMetadata{
        "vision.detect-v1",
        "Vision",
        "1.0.0",
        "A deterministic test plugin.",
        false,
    };
}

TEST(PluginMetadataTest, AcceptsCanonicalOperationAndUtf8ValueFields) {
    auto metadata = valid_metadata();
    metadata.name = "工业插件";

    EXPECT_TRUE(validate_metadata(metadata, PluginLimits::create().value()));
}

TEST(PluginMetadataTest, RejectsEmptyOperation) {
    auto metadata = valid_metadata();
    metadata.operation.clear();

    const auto result =
        validate_metadata(metadata, PluginLimits::create().value());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PluginMetadataTest, RejectsUppercaseAndUnsupportedOperationCharacters) {
    for (const std::string operation :
         {"Echo", "echo/plugin", "echo plugin", "écho"}) {
        auto metadata = valid_metadata();
        metadata.operation = operation;
        EXPECT_FALSE(
            validate_metadata(metadata, PluginLimits::create().value()));
    }
}

TEST(PluginMetadataTest, RejectsLeadingTrailingAndEmptyDotSegments) {
    for (const std::string operation : {".echo", "echo.", "echo..run"}) {
        auto metadata = valid_metadata();
        metadata.operation = operation;
        EXPECT_FALSE(
            validate_metadata(metadata, PluginLimits::create().value()));
    }
}

TEST(PluginMetadataTest, RejectsEmptyDisplayFields) {
    auto name = valid_metadata();
    name.name.clear();
    auto version = valid_metadata();
    version.version.clear();
    auto description = valid_metadata();
    description.description.clear();

    const auto limits = PluginLimits::create().value();
    EXPECT_FALSE(validate_metadata(name, limits));
    EXPECT_FALSE(validate_metadata(version, limits));
    EXPECT_FALSE(validate_metadata(description, limits));
}

TEST(PluginMetadataTest, RejectsControlCharactersAndNul) {
    auto name = valid_metadata();
    name.name = std::string{"bad\0name", 8};
    auto version = valid_metadata();
    version.version = "1\n0";
    auto description = valid_metadata();
    description.description = "bad\ttext";

    const auto limits = PluginLimits::create().value();
    EXPECT_FALSE(validate_metadata(name, limits));
    EXPECT_FALSE(validate_metadata(version, limits));
    EXPECT_FALSE(validate_metadata(description, limits));
}

TEST(PluginMetadataTest, EnforcesUtf8ByteLimitsWithoutClamping) {
    auto metadata = valid_metadata();
    metadata.name = "工业";
    const auto five_bytes = PluginLimits::create(1, 32, 5, 32, 64, 32).value();
    const auto six_bytes = PluginLimits::create(1, 32, 6, 32, 64, 32).value();

    EXPECT_FALSE(validate_metadata(metadata, five_bytes));
    EXPECT_TRUE(validate_metadata(metadata, six_bytes));
}

TEST(PluginMetadataTest, RejectsMalformedUtf8) {
    auto metadata = valid_metadata();
    metadata.description = std::string{"\xC0\xAF", 2};

    EXPECT_FALSE(
        validate_metadata(metadata, PluginLimits::create().value()));
}

TEST(PluginMetadataTest, EnforcesEveryMetadataFieldLimit) {
    auto metadata = valid_metadata();
    const auto limits = PluginLimits::create(2, 3, 3, 3, 3, 8).value();

    EXPECT_FALSE(validate_metadata(metadata, limits));
}

TEST(PluginMetadataTest, AcceptsCanonicalUniqueCapabilities) {
    auto metadata = valid_metadata();
    metadata.capabilities = {
        "deterministic",
        "vision.mock-v1",
        "thread_safe",
    };

    EXPECT_TRUE(
        validate_metadata(metadata, PluginLimits::create().value()));
}

TEST(PluginMetadataTest, RejectsInvalidAndDuplicateCapabilities) {
    const auto limits = PluginLimits::create().value();
    for (const std::string capability :
         {"", "Uppercase", ".leading", "bad space", "bad..segment"}) {
        auto metadata = valid_metadata();
        metadata.capabilities = {capability};
        EXPECT_FALSE(validate_metadata(metadata, limits));
    }

    auto duplicate = valid_metadata();
    duplicate.capabilities = {"mock", "mock"};
    EXPECT_FALSE(validate_metadata(duplicate, limits));
}

TEST(PluginMetadataTest, EnforcesCapabilityCountAndByteLimits) {
    const auto limits = PluginLimits::create(
        2,
        32,
        32,
        32,
        64,
        32,
        64,
        64,
        8,
        32,
        32,
        1,
        4).value();
    auto too_many = valid_metadata();
    too_many.capabilities = {"one", "two"};
    auto too_long = valid_metadata();
    too_long.capabilities = {"12345"};

    EXPECT_FALSE(validate_metadata(too_many, limits));
    EXPECT_FALSE(validate_metadata(too_long, limits));
}

}  // namespace
