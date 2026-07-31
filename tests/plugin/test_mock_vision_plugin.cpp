#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/plugin/mock_vision_plugin.hpp"

namespace {

using namespace std::chrono_literals;
using iaisf::ErrorCode;
using iaisf::plugin::MockVisionPlugin;

nlohmann::json valid_input() {
    return {
        {"image_id", "demo-001"},
        {"width", 640},
        {"height", 480},
    };
}

TEST(MockVisionPluginTest, MetadataTruthfullyDeclaresMockBehavior) {
    const MockVisionPlugin plugin;
    const auto metadata = plugin.metadata();

    EXPECT_EQ(metadata.operation, "mock_vision.detect");
    EXPECT_TRUE(metadata.mock);
    EXPECT_NE(metadata.description.find("mock"), std::string::npos);
    EXPECT_NE(metadata.description.find("no real inference"), std::string::npos);
    EXPECT_NE(
        std::find(
            metadata.capabilities.begin(),
            metadata.capabilities.end(),
            "mock"),
        metadata.capabilities.end());
}

TEST(MockVisionPluginTest, DefaultThresholdProducesDeterministicDetection) {
    const MockVisionPlugin plugin;

    const auto result = plugin.execute(valid_input());

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().at("mock").get<bool>());
    EXPECT_EQ(result.value().at("operation"), "mock_vision.detect");
    EXPECT_EQ(result.value().at("image_id"), "demo-001");
    EXPECT_EQ(result.value().at("image_size").at("width"), 640);
    ASSERT_EQ(result.value().at("detections").size(), 1U);
    EXPECT_EQ(
        result.value().at("detections").at(0).at("type"),
        "weld_seam");
}

TEST(MockVisionPluginTest, ThresholdAtConfidenceIncludesDetection) {
    const MockVisionPlugin plugin;
    auto input = valid_input();
    input["confidence_threshold"] = 0.93;

    const auto result = plugin.execute(input);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().at("detections").size(), 1U);
}

TEST(MockVisionPluginTest, ThresholdAboveConfidenceReturnsNoDetection) {
    const MockVisionPlugin plugin;
    auto input = valid_input();
    input["confidence_threshold"] = 0.94;

    const auto result = plugin.execute(input);

    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().at("detections").empty());
    EXPECT_TRUE(result.value().at("mock").get<bool>());
}

TEST(MockVisionPluginTest, BboxUsesDeterministicIntegerRule) {
    const MockVisionPlugin plugin;

    const auto input = valid_input();
    ASSERT_TRUE(plugin.validate_input(input));
    const auto first = plugin.execute(input);
    const auto second = plugin.execute(input);

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), second.value());
    const auto& bbox =
        first.value().at("detections").at(0).at("bbox");
    EXPECT_EQ(bbox.at("x"), 160);
    EXPECT_EQ(bbox.at("y"), 160);
    EXPECT_EQ(bbox.at("width"), 320);
    EXPECT_EQ(bbox.at("height"), 48);
}

TEST(MockVisionPluginTest, ValidationIsRepeatableAndSideEffectFree) {
    const MockVisionPlugin plugin;
    auto valid = valid_input();
    auto invalid = valid_input();
    invalid["width"] = 0;
    const auto valid_original = valid;
    const auto invalid_original = invalid;

    EXPECT_TRUE(plugin.validate_input(valid));
    EXPECT_TRUE(plugin.validate_input(valid));
    const auto first_invalid = plugin.validate_input(invalid);
    const auto second_invalid = plugin.validate_input(invalid);

    ASSERT_FALSE(first_invalid);
    ASSERT_FALSE(second_invalid);
    EXPECT_EQ(first_invalid.error().code, second_invalid.error().code);
    EXPECT_EQ(first_invalid.error().message, second_invalid.error().message);
    EXPECT_EQ(valid, valid_original);
    EXPECT_EQ(invalid, invalid_original);
}

TEST(MockVisionPluginTest, RequiresObjectAndImageId) {
    const MockVisionPlugin plugin;
    auto missing = valid_input();
    missing.erase("image_id");

    EXPECT_FALSE(plugin.validate_input(nlohmann::json::array()));
    EXPECT_FALSE(plugin.validate_input(missing));
    auto wrong_type = valid_input();
    wrong_type["image_id"] = 1;
    EXPECT_FALSE(plugin.validate_input(wrong_type));
}

TEST(MockVisionPluginTest, RejectsEmptyControlAndOversizedImageId) {
    const MockVisionPlugin plugin;
    auto empty = valid_input();
    empty["image_id"] = "";
    auto control = valid_input();
    control["image_id"] = std::string{"bad\0id", 6};
    auto oversized = valid_input();
    oversized["image_id"] = std::string(129, 'x');

    EXPECT_FALSE(plugin.validate_input(empty));
    EXPECT_FALSE(plugin.validate_input(control));
    EXPECT_FALSE(plugin.validate_input(oversized));
}

TEST(MockVisionPluginTest, RequiresBothDimensions) {
    const MockVisionPlugin plugin;
    auto no_width = valid_input();
    no_width.erase("width");
    auto no_height = valid_input();
    no_height.erase("height");

    EXPECT_FALSE(plugin.validate_input(no_width));
    EXPECT_FALSE(plugin.validate_input(no_height));
}

TEST(MockVisionPluginTest, RejectsDimensionsOutsideRange) {
    const MockVisionPlugin plugin;
    for (const nlohmann::json value : {-1, 0, 16385}) {
        auto width = valid_input();
        width["width"] = value;
        auto height = valid_input();
        height["height"] = value;
        EXPECT_FALSE(plugin.validate_input(width));
        EXPECT_FALSE(plugin.validate_input(height));
    }
}

TEST(MockVisionPluginTest, RejectsNonIntegerDimensions) {
    const MockVisionPlugin plugin;
    const std::vector<nlohmann::json> invalid{
        true,
        1.5,
        "640",
        nullptr,
    };
    for (const auto& value : invalid) {
        auto input = valid_input();
        input["width"] = value;
        EXPECT_FALSE(plugin.validate_input(input));
    }
}

TEST(MockVisionPluginTest, AcceptsDimensionBoundaries) {
    const MockVisionPlugin plugin;
    auto minimum = valid_input();
    minimum["width"] = 1;
    minimum["height"] = 1;
    auto maximum = valid_input();
    maximum["width"] = 16384;
    maximum["height"] = 16384;

    EXPECT_TRUE(plugin.validate_input(minimum));
    EXPECT_TRUE(plugin.validate_input(maximum));
}

TEST(MockVisionPluginTest, RejectsInvalidThresholdTypesAndRanges) {
    const MockVisionPlugin plugin;
    const std::vector<nlohmann::json> invalid{
        "0.5",
        nullptr,
        -0.01,
        1.01,
    };
    for (const auto& value : invalid) {
        auto input = valid_input();
        input["confidence_threshold"] = value;
        EXPECT_FALSE(plugin.validate_input(input));
    }
}

TEST(MockVisionPluginTest, RejectsNonFiniteThreshold) {
    const MockVisionPlugin plugin;
    auto input = valid_input();
    input["confidence_threshold"] =
        std::numeric_limits<double>::infinity();

    EXPECT_FALSE(plugin.validate_input(input));
}

TEST(MockVisionPluginTest, RejectsUnknownAndPathLikeFields) {
    const MockVisionPlugin plugin;
    auto unknown = valid_input();
    unknown["extra"] = true;
    auto path = valid_input();
    path["image_path"] = "data/image.png";

    EXPECT_FALSE(plugin.validate_input(unknown));
    EXPECT_FALSE(plugin.validate_input(path));
}

TEST(MockVisionPluginTest, OutputContainsNoModelTimingOrVerificationClaims) {
    const MockVisionPlugin plugin;
    const auto result = plugin.execute(valid_input());

    ASSERT_TRUE(result);
    EXPECT_FALSE(result.value().contains("inference_time_ms"));
    EXPECT_FALSE(result.value().contains("model_accuracy"));
    EXPECT_FALSE(result.value().contains("production_ready"));
    EXPECT_FALSE(result.value().contains("verified_detection"));
}

TEST(MockVisionPluginTest, ConcurrentExecutionIsStableAndIndependent) {
    const MockVisionPlugin plugin;
    std::vector<std::future<nlohmann::json>> futures;
    for (int index = 0; index < 12; ++index) {
        futures.push_back(std::async(
            std::launch::async,
            [&plugin, index] {
                auto input = valid_input();
                input["image_id"] = "image-" + std::to_string(index);
                auto result = plugin.execute(input);
                return result ? std::move(result).value()
                              : nlohmann::json(nullptr);
            }));
    }

    for (int index = 0; index < 12; ++index) {
        auto& future = futures[static_cast<std::size_t>(index)];
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        const auto output = future.get();
        EXPECT_TRUE(output.at("mock").get<bool>());
        EXPECT_EQ(output.at("image_id"), "image-" + std::to_string(index));
    }
}

TEST(MockVisionPluginTest, ValidationAndExecutionCanRunConcurrently) {
    const MockVisionPlugin plugin;
    std::vector<std::future<bool>> futures;
    for (int index = 0; index < 20; ++index) {
        futures.push_back(std::async(
            std::launch::async,
            [&plugin, index] {
                auto input = valid_input();
                input["image_id"] = "parallel-" + std::to_string(index);
                if (index % 2 == 0) {
                    return static_cast<bool>(plugin.validate_input(input));
                }
                const auto output = plugin.execute(input);
                return output &&
                       output.value().at("image_id") ==
                           "parallel-" + std::to_string(index);
            }));
    }

    for (auto& future : futures) {
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_TRUE(future.get());
    }
}

}  // namespace
