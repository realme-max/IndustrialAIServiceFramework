#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/plugin/echo_plugin.hpp"

namespace {

using namespace std::chrono_literals;
using iaisf::ErrorCode;
using iaisf::plugin::EchoPlugin;

TEST(EchoPluginTest, MetadataIsNonMockAndCanonical) {
    const EchoPlugin plugin;
    const auto metadata = plugin.metadata();

    EXPECT_EQ(metadata.operation, "echo");
    EXPECT_FALSE(metadata.mock);
    EXPECT_FALSE(metadata.name.empty());
    EXPECT_FALSE(metadata.version.empty());
    EXPECT_FALSE(metadata.description.empty());
    EXPECT_EQ(metadata.capabilities.size(), 2U);
}

TEST(EchoPluginTest, RequiresObjectInput) {
    const EchoPlugin plugin;

    const auto result = plugin.validate_input(nlohmann::json::array());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(EchoPluginTest, RequiresPayload) {
    const EchoPlugin plugin;

    EXPECT_FALSE(plugin.validate_input(nlohmann::json::object()));
}

TEST(EchoPluginTest, RejectsUnknownFields) {
    const EchoPlugin plugin;

    EXPECT_FALSE(plugin.validate_input(
        {{"payload", 1}, {"unexpected", true}}));
}

TEST(EchoPluginTest, AcceptsEveryJsonPayloadKind) {
    const EchoPlugin plugin;
    const std::vector<nlohmann::json> values{
        nullptr,
        true,
        false,
        -42,
        std::uint64_t{42},
        3.5,
        "text",
        nlohmann::json::array({1, 2}),
        nlohmann::json::object({{"nested", "value"}}),
    };

    for (const auto& value : values) {
        const nlohmann::json input{{"payload", value}};
        const auto result = plugin.execute(input);
        ASSERT_TRUE(result);
        EXPECT_EQ(result.value(), value);
    }
}

TEST(EchoPluginTest, PreservesBinarySafeJsonString) {
    const EchoPlugin plugin;
    const std::string bytes{"a\0b", 3};

    const auto result = plugin.execute({{"payload", bytes}});

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().get<std::string>(), bytes);
}

TEST(EchoPluginTest, IsDeterministicAndDoesNotMutateInput) {
    const EchoPlugin plugin;
    nlohmann::json input{{"payload", {{"value", 7}}}};
    const auto original = input;

    ASSERT_TRUE(plugin.validate_input(input));
    const auto first = plugin.execute(input);
    const auto second = plugin.execute(input);

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value(), second.value());
    EXPECT_EQ(first.value(), input.at("payload"));
    EXPECT_EQ(input, original);
}

TEST(EchoPluginTest, ReturnedPayloadIsIndependentFromLaterInputMutation) {
    const EchoPlugin plugin;
    nlohmann::json input{
        {"payload", {{"array", nlohmann::json::array({1, 2, 3})}}},
    };

    const auto result = plugin.execute(input);
    ASSERT_TRUE(result);
    input["payload"]["array"][0] = 99;
    input["payload"]["new"] = true;

    EXPECT_EQ(
        result.value(),
        nlohmann::json({{"array", nlohmann::json::array({1, 2, 3})}}));
}

TEST(EchoPluginTest, ValidationIsRepeatableAndSideEffectFree) {
    const EchoPlugin plugin;
    nlohmann::json valid{{"payload", {{"value", 7}}}};
    nlohmann::json invalid{{"wrong", 7}};
    const auto valid_original = valid;
    const auto invalid_original = invalid;

    const auto first_valid = plugin.validate_input(valid);
    const auto second_valid = plugin.validate_input(valid);
    const auto first_invalid = plugin.validate_input(invalid);
    const auto second_invalid = plugin.validate_input(invalid);

    EXPECT_TRUE(first_valid);
    EXPECT_TRUE(second_valid);
    ASSERT_FALSE(first_invalid);
    ASSERT_FALSE(second_invalid);
    EXPECT_EQ(first_invalid.error().code, second_invalid.error().code);
    EXPECT_EQ(first_invalid.error().message, second_invalid.error().message);
    EXPECT_EQ(valid, valid_original);
    EXPECT_EQ(invalid, invalid_original);
}

TEST(EchoPluginTest, ConcurrentExecutionsProduceIndependentResults) {
    const EchoPlugin plugin;
    std::vector<std::future<nlohmann::json>> futures;
    for (int index = 0; index < 16; ++index) {
        futures.push_back(std::async(
            std::launch::async,
            [&plugin, index] {
                auto result = plugin.execute({{"payload", index}});
                return result ? std::move(result).value()
                              : nlohmann::json(nullptr);
            }));
    }

    for (int index = 0; index < 16; ++index) {
        auto& future = futures[static_cast<std::size_t>(index)];
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        const auto output = future.get();
        EXPECT_EQ(output, index);
    }
}

TEST(EchoPluginTest, ValidationAndExecutionCanRunConcurrently) {
    const EchoPlugin plugin;
    std::vector<std::future<bool>> futures;
    for (int index = 0; index < 24; ++index) {
        futures.push_back(std::async(
            std::launch::async,
            [&plugin, index] {
                const nlohmann::json input{{"payload", index}};
                if (index % 2 == 0) {
                    return static_cast<bool>(plugin.validate_input(input));
                }
                const auto output = plugin.execute(input);
                return output && output.value() == index;
            }));
    }

    for (auto& future : futures) {
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_TRUE(future.get());
    }
}

TEST(EchoPluginTest, ExecuteRejectsInvalidInputWithoutThrowing) {
    const EchoPlugin plugin;

    const auto result = plugin.execute({{"wrong", "field"}});

    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

}  // namespace
