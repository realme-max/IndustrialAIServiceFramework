#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "iaisf/core/json_value_limits.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/mock_vision_plugin.hpp"
#include "iaisf/plugin/plugin_manager.hpp"

namespace {

using namespace std::chrono_literals;
using iaisf::ErrorCode;
using iaisf::JsonValueLimits;
using iaisf::Result;
using iaisf::make_error;
using iaisf::validate_json_value;
using iaisf::plugin::EchoPlugin;
using iaisf::plugin::IAlgorithmPlugin;
using iaisf::plugin::MockVisionPlugin;
using iaisf::plugin::PluginLimits;
using iaisf::plugin::PluginManager;
using iaisf::plugin::PluginMetadata;

class FunctionPlugin final : public IAlgorithmPlugin {
public:
    using Validator =
        std::function<Result<void>(const nlohmann::json&)>;
    using Executor =
        std::function<Result<nlohmann::json>(const nlohmann::json&)>;

    explicit FunctionPlugin(
        PluginMetadata metadata,
        Validator validator = [](const nlohmann::json&) {
            return Result<void>::success();
        },
        Executor executor = [](const nlohmann::json& input) {
            return Result<nlohmann::json>::success(input);
        })
        : metadata_value(std::move(metadata)),
          validator_(std::move(validator)),
          executor_(std::move(executor)) {
        if (!validator_) {
            validator_ = [](const nlohmann::json&) {
                return Result<void>::success();
            };
        }
        if (!executor_) {
            executor_ = [](const nlohmann::json& input) {
                return Result<nlohmann::json>::success(input);
            };
        }
    }

    PluginMetadata metadata() const override {
        metadata_calls.fetch_add(1, std::memory_order_relaxed);
        if (metadata_hook) {
            metadata_hook();
        }
        if (throw_metadata_standard) {
            throw std::runtime_error("secret metadata path");
        }
        if (throw_metadata_unknown) {
            throw 7;
        }
        return metadata_value;
    }

    Result<void> validate_input(
        const nlohmann::json& input) const override {
        validation_calls.fetch_add(1, std::memory_order_relaxed);
        return validator_(input);
    }

    Result<nlohmann::json> execute(
        const nlohmann::json& input) const override {
        execution_calls.fetch_add(1, std::memory_order_relaxed);
        return executor_(input);
    }

    mutable std::atomic<int> metadata_calls{0};
    mutable std::atomic<int> validation_calls{0};
    mutable std::atomic<int> execution_calls{0};
    PluginMetadata metadata_value;
    bool throw_metadata_standard{false};
    bool throw_metadata_unknown{false};
    std::function<void()> metadata_hook;

private:
    Validator validator_;
    Executor executor_;
};

PluginMetadata metadata_for(const std::string& operation) {
    return PluginMetadata{
        operation,
        "Test Plugin",
        "1.0.0",
        "Test-only deterministic plugin.",
        false,
    };
}

std::shared_ptr<PluginManager> make_manager(
    PluginLimits limits = PluginLimits::create().value()) {
    return std::make_shared<PluginManager>(std::move(limits));
}

TEST(PluginManagerTest, RejectsNullPluginWithoutChangingRegistry) {
    const auto manager = make_manager();

    const auto result =
        manager->register_plugin(std::shared_ptr<const IAlgorithmPlugin>{});

    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(manager->size(), 0U);
}

TEST(PluginManagerTest, RegistersMetadataExactlyOnceAndStoresCopy) {
    const auto manager = make_manager();
    auto plugin_metadata = metadata_for("copy.test");
    plugin_metadata.capabilities = {"stable"};
    const auto plugin =
        std::make_shared<FunctionPlugin>(std::move(plugin_metadata));

    ASSERT_TRUE(manager->register_plugin(plugin));
    EXPECT_EQ(plugin->metadata_calls.load(std::memory_order_relaxed), 1);
    plugin->metadata_value.name = "mutated";
    plugin->metadata_value.capabilities.push_back("mutated");
    ASSERT_TRUE(manager->freeze());
    const auto metadata = manager->find_metadata("copy.test");

    ASSERT_TRUE(metadata);
    EXPECT_EQ(metadata.value().name, "Test Plugin");
    ASSERT_EQ(metadata.value().capabilities.size(), 1U);
    EXPECT_EQ(metadata.value().capabilities.front(), "stable");
    EXPECT_EQ(plugin->metadata_calls.load(std::memory_order_relaxed), 1);
}

TEST(PluginManagerTest, DuplicateOperationDoesNotReplaceOriginal) {
    const auto manager = make_manager();
    const auto first =
        std::make_shared<FunctionPlugin>(metadata_for("duplicate"));
    auto second_metadata = metadata_for("duplicate");
    second_metadata.name = "Replacement";
    const auto second =
        std::make_shared<FunctionPlugin>(std::move(second_metadata));

    ASSERT_TRUE(manager->register_plugin(first));
    const auto duplicate = manager->register_plugin(second);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(manager->size(), 1U);
    ASSERT_TRUE(manager->freeze());
    EXPECT_EQ(
        manager->find_metadata("duplicate").value().name,
        "Test Plugin");
}

TEST(PluginManagerTest, EnforcesRegistryCapacityTransactionally) {
    const auto manager = make_manager(PluginLimits::create(1).value());

    ASSERT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("first"))));
    const auto full = manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("second")));

    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(manager->size(), 1U);
}

TEST(PluginManagerTest, InvalidMetadataLeavesManagerUsable) {
    const auto manager = make_manager();
    auto invalid = metadata_for("Bad.Operation");

    const auto rejected = manager->register_plugin(
        std::make_shared<FunctionPlugin>(std::move(invalid)));

    EXPECT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(manager->size(), 0U);
    EXPECT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("valid"))));
}

TEST(PluginManagerTest, StandardMetadataExceptionIsSanitized) {
    const auto manager = make_manager();
    const auto plugin =
        std::make_shared<FunctionPlugin>(metadata_for("throwing"));
    plugin->throw_metadata_standard = true;

    const auto rejected = manager->register_plugin(plugin);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InternalError);
    EXPECT_EQ(rejected.error().message, "plugin metadata failed");
    EXPECT_EQ(rejected.error().message.find("secret"), std::string::npos);
    EXPECT_EQ(manager->size(), 0U);
}

TEST(PluginManagerTest, UnknownMetadataExceptionIsSanitizedAndRecoveryWorks) {
    const auto manager = make_manager();
    const auto plugin =
        std::make_shared<FunctionPlugin>(metadata_for("throwing"));
    plugin->throw_metadata_unknown = true;

    const auto rejected = manager->register_plugin(plugin);

    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InternalError);
    EXPECT_EQ(manager->size(), 0U);
    EXPECT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("recovery"))));
}

TEST(PluginManagerTest, FreezeIsIdempotentAndPermanentlyRejectsRegistration) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("before"))));

    EXPECT_TRUE(manager->freeze());
    EXPECT_TRUE(manager->freeze());
    EXPECT_TRUE(manager->frozen());
    const auto after_plugin =
        std::make_shared<FunctionPlugin>(metadata_for("after"));
    const auto after = manager->register_plugin(after_plugin);
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(
        after_plugin->metadata_calls.load(std::memory_order_relaxed),
        0);
    EXPECT_EQ(manager->size(), 1U);
}

TEST(PluginManagerTest, ReadAndExecutionApisRequireFrozenRegistry) {
    const auto manager = make_manager();
    const auto plugin =
        std::make_shared<FunctionPlugin>(metadata_for("operation"));
    ASSERT_TRUE(manager->register_plugin(plugin));

    EXPECT_EQ(
        manager->find_metadata("operation").error().code,
        ErrorCode::InvalidState);
    EXPECT_EQ(
        manager->list_metadata().error().code,
        ErrorCode::InvalidState);
    EXPECT_EQ(
        manager->validate("operation", {}).error().code,
        ErrorCode::InvalidState);
    EXPECT_EQ(
        manager->execute("operation", {}).error().code,
        ErrorCode::InvalidState);
    EXPECT_EQ(plugin->validation_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(plugin->execution_calls.load(std::memory_order_relaxed), 0);

    ASSERT_TRUE(manager->freeze());
    EXPECT_TRUE(manager->list_metadata());
    EXPECT_TRUE(manager->find_metadata("operation"));
    EXPECT_TRUE(manager->validate("operation", {}));
    EXPECT_TRUE(manager->execute("operation", {}));
    EXPECT_EQ(plugin->validation_calls.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(plugin->execution_calls.load(std::memory_order_relaxed), 1);
}

TEST(PluginManagerTest, UnknownOperationUsesStructuredNotFound) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->freeze());

    EXPECT_EQ(
        manager->find_metadata("missing").error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(
        manager->validate("missing", {}).error().code,
        ErrorCode::NotFound);
    EXPECT_EQ(
        manager->execute("missing", {}).error().code,
        ErrorCode::NotFound);
}

TEST(PluginManagerTest, ListMetadataReturnsIndependentStableSortedCopy) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("zeta"))));
    ASSERT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("alpha"))));
    ASSERT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("middle"))));
    ASSERT_TRUE(manager->freeze());

    auto list = manager->list_metadata();
    ASSERT_TRUE(list);
    ASSERT_EQ(list.value().size(), 3U);
    EXPECT_EQ(list.value()[0].operation, "alpha");
    EXPECT_EQ(list.value()[1].operation, "middle");
    EXPECT_EQ(list.value()[2].operation, "zeta");
    list.value()[0].name = "changed";
    EXPECT_EQ(manager->find_metadata("alpha").value().name, "Test Plugin");
}

TEST(PluginManagerTest, ValidationExceptionsAreFixedAndDoNotLeakText) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("standard"),
        [](const nlohmann::json&) -> Result<void> {
            throw std::runtime_error("validation secret");
        })));
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("unknown"),
        [](const nlohmann::json&) -> Result<void> { throw 9; })));
    ASSERT_TRUE(manager->freeze());

    const auto standard = manager->validate("standard", {});
    const auto unknown = manager->validate("unknown", {});
    ASSERT_FALSE(standard);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(standard.error().code, ErrorCode::InternalError);
    EXPECT_EQ(standard.error().message, "plugin validation failed");
    EXPECT_EQ(unknown.error().message, "plugin validation failed");
}

TEST(PluginManagerTest, ExecutionExceptionsAreFixedAndDoNotLeakText) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("standard"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) -> Result<nlohmann::json> {
            throw std::runtime_error("C:\\secret\\model.bin");
        })));
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("unknown"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) -> Result<nlohmann::json> { throw 11; })));
    ASSERT_TRUE(manager->freeze());

    const auto standard = manager->execute("standard", {});
    const auto unknown = manager->execute("unknown", {});
    ASSERT_FALSE(standard);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(standard.error().code, ErrorCode::InternalError);
    EXPECT_EQ(standard.error().message, "plugin execution failed");
    EXPECT_EQ(standard.error().message.find("secret"), std::string::npos);
    EXPECT_EQ(unknown.error().message, "plugin execution failed");
}

TEST(PluginManagerTest, ReturnedInternalPluginErrorsAreSanitized) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("failure"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::failure(make_error(
                ErrorCode::IoError,
                "C:\\private\\input.bin"));
        })));
    ASSERT_TRUE(manager->freeze());

    const auto failed = manager->execute("failure", {});

    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::InternalError);
    EXPECT_EQ(failed.error().message, "plugin execution failed");
}

TEST(PluginManagerTest, ValidationErrorsAreBoundedAndExecutionErrorsAreFixed) {
    const auto manager = make_manager(
        PluginLimits::create(2, 64, 64, 64, 128, 12).value());
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("invalid"),
        [](const nlohmann::json&) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "safe input"));
        },
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success({});
        })));
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("execution"),
        [](const nlohmann::json&) {
            return Result<void>::success();
        },
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::failure(make_error(
                ErrorCode::ResourceExhausted,
                "safe limit"));
        })));
    ASSERT_TRUE(manager->freeze());

    EXPECT_EQ(
        manager->validate("invalid", {}).error().message,
        "safe input");
    const auto execution = manager->execute("execution", {});
    ASSERT_FALSE(execution);
    EXPECT_EQ(execution.error().code, ErrorCode::InternalError);
    EXPECT_EQ(execution.error().message, "plugin execu");
}

TEST(PluginManagerTest, EnforcesInputJsonCapacityBeforePluginValidation) {
    const auto limits = PluginLimits::create(
        2, 64, 64, 64, 128, 64, 64, 64, 2, 3, 4, 8, 32).value();
    std::atomic<int> validation_calls{0};
    const auto manager = make_manager(limits);
    const auto plugin = std::make_shared<FunctionPlugin>(
        metadata_for("bounded"),
        [&validation_calls](const nlohmann::json&) {
            validation_calls.fetch_add(1, std::memory_order_relaxed);
            return Result<void>::success();
        });
    ASSERT_TRUE(manager->register_plugin(plugin));
    ASSERT_TRUE(manager->freeze());

    EXPECT_TRUE(manager->validate("bounded", {{"key", "1234"}}));
    EXPECT_TRUE(manager->validate(
        "bounded",
        nlohmann::json::array({1, 2})));
    const auto too_deep =
        manager->validate("bounded", {{"key", {{"nested", 1}}}});
    const auto too_many = manager->validate(
        "bounded",
        nlohmann::json::array({1, 2, 3}));
    const auto too_wide_object = manager->validate(
        "bounded",
        {{"one", 1}, {"two", 2}, {"three", 3}});
    const auto too_long =
        manager->validate("bounded", {{"key", "12345"}});
    const auto non_finite = manager->validate(
        "bounded",
        std::numeric_limits<double>::infinity());
    const auto discarded = manager->validate(
        "bounded",
        nlohmann::json::parse("not-json", nullptr, false));
    const auto malformed_utf8 = manager->validate(
        "bounded",
        nlohmann::json(std::string(1, static_cast<char>(0xFF))));
    const auto binary = manager->validate(
        "bounded",
        nlohmann::json::binary({0x01U}));

    ASSERT_FALSE(too_deep);
    ASSERT_FALSE(too_many);
    ASSERT_FALSE(too_wide_object);
    ASSERT_FALSE(too_long);
    ASSERT_FALSE(non_finite);
    ASSERT_FALSE(discarded);
    ASSERT_FALSE(malformed_utf8);
    ASSERT_FALSE(binary);
    EXPECT_EQ(too_deep.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(too_many.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(
        too_wide_object.error().code,
        ErrorCode::ResourceExhausted);
    EXPECT_EQ(too_long.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(non_finite.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(discarded.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(malformed_utf8.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(binary.error().code, ErrorCode::InvalidArgument);
    const auto healthy =
        manager->execute("bounded", {{"key", "ok"}});
    ASSERT_TRUE(healthy);
    EXPECT_EQ(healthy.value().at("key"), "ok");
    EXPECT_EQ(validation_calls.load(std::memory_order_relaxed), 3);
    EXPECT_EQ(plugin->execution_calls.load(std::memory_order_relaxed), 1);
}

TEST(PluginManagerTest, SerializedInputAndOutputByteLimitsAreInclusive) {
    const auto limits = PluginLimits::create(
        2, 64, 64, 64, 128, 64, 5, 5, 8, 32, 32, 8, 32).value();
    const auto manager = make_manager(limits);
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("exact"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success("abc");
        })));
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("output.large"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success("abcd");
        })));
    ASSERT_TRUE(manager->freeze());

    EXPECT_TRUE(manager->validate("exact", "abc"));
    EXPECT_TRUE(manager->execute("exact", "abc"));
    const auto input_large = manager->validate("exact", "abcd");
    const auto output_large = manager->execute("output.large", {});
    ASSERT_FALSE(input_large);
    ASSERT_FALSE(output_large);
    EXPECT_EQ(input_large.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(output_large.error().code, ErrorCode::ResourceExhausted);
}

TEST(
    PluginManagerTest,
    CompactSerializedByteCounterMatchesEscapesUtf8KeysAndPunctuation) {
    const std::vector<nlohmann::json> values{
        "plain ascii",
        std::string(12, '"'),
        std::string(12, '\\'),
        std::string{"line\ncolumn\treturn\r"},
        std::string{"\xE7\x84\x8A\xE7\xBC\x9D"},
        nlohmann::json{
            {"quoted\"key\\with\ncontrol", "value"},
        },
        nlohmann::json{
            {
                "nested",
                nlohmann::json::array(
                    {nullptr, true, false, 42, -7, 6.25, {{"x", "y"}}}),
            },
        },
    };

    for (const auto& value : values) {
        const auto expected = value.dump().size();
        const auto exact = validate_json_value(
            value,
            JsonValueLimits{expected, 32, 128, 4096},
            "test output",
            ErrorCode::InternalError);
        ASSERT_TRUE(exact);
        EXPECT_EQ(exact.value(), expected);

        ASSERT_GT(expected, 1U);
        const auto one_byte_short = validate_json_value(
            value,
            JsonValueLimits{expected - 1U, 32, 128, 4096},
            "test output",
            ErrorCode::InternalError);
        ASSERT_FALSE(one_byte_short);
        EXPECT_EQ(
            one_byte_short.error().code,
            ErrorCode::ResourceExhausted);
    }
}

TEST(
    PluginManagerTest,
    EscapingCanExceedSerializedLimitWhenRawStringFits) {
    const std::string raw{"\n\t\r"};
    ASSERT_EQ(raw.size(), 3U);
    ASSERT_GT(nlohmann::json(raw).dump().size(), raw.size());

    const auto result = validate_json_value(
        nlohmann::json(raw),
        JsonValueLimits{raw.size(), 4, 4, raw.size()},
        "test output",
        ErrorCode::InternalError);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
}

TEST(
    PluginManagerTest,
    EchoAndMockVisionUseExactCompactOutputByteLimit) {
    const nlohmann::json echo_input{
        {"payload", "quote\"slash\\line\n\xE7\x84\x8A\xE7\xBC\x9D"},
    };
    const EchoPlugin echo;
    const auto echo_output = echo.execute(echo_input);
    ASSERT_TRUE(echo_output);
    const auto echo_bytes =
        static_cast<std::int64_t>(echo_output.value().dump().size());

    const nlohmann::json vision_input{
        {"image_id", "demo"},
        {"width", 640},
        {"height", 480},
    };
    const MockVisionPlugin vision;
    const auto vision_output = vision.execute(vision_input);
    ASSERT_TRUE(vision_output);
    const auto vision_bytes =
        static_cast<std::int64_t>(vision_output.value().dump().size());

    const auto execute_with_limit = [](
                                        std::shared_ptr<const IAlgorithmPlugin>
                                            plugin,
                                        const std::string& operation,
                                        const nlohmann::json& input,
                                        const std::int64_t output_bytes) {
        auto manager = make_manager(PluginLimits::create(
            2,
            128,
            128,
            64,
            1024,
            128,
            4096,
            output_bytes,
            64,
            1000,
            4096,
            16,
            128).value());
        EXPECT_TRUE(manager->register_plugin(std::move(plugin)));
        EXPECT_TRUE(manager->freeze());
        return manager->execute(operation, input);
    };

    EXPECT_TRUE(execute_with_limit(
        std::make_shared<EchoPlugin>(),
        "echo",
        echo_input,
        echo_bytes));
    const auto echo_short = execute_with_limit(
        std::make_shared<EchoPlugin>(),
        "echo",
        echo_input,
        echo_bytes - 1);
    ASSERT_FALSE(echo_short);
    EXPECT_EQ(echo_short.error().code, ErrorCode::ResourceExhausted);

    EXPECT_TRUE(execute_with_limit(
        std::make_shared<MockVisionPlugin>(),
        "mock_vision.detect",
        vision_input,
        vision_bytes));
    const auto vision_short = execute_with_limit(
        std::make_shared<MockVisionPlugin>(),
        "mock_vision.detect",
        vision_input,
        vision_bytes - 1);
    ASSERT_FALSE(vision_short);
    EXPECT_EQ(vision_short.error().code, ErrorCode::ResourceExhausted);
}

TEST(PluginManagerTest, EnforcesOutputStructureAndFiniteNumbers) {
    const auto limits = PluginLimits::create(
        5, 64, 64, 64, 128, 64, 64, 64, 2, 3, 4, 8, 32).value();
    const auto manager = make_manager(limits);
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("deep"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success(
                nlohmann::json{{"key", {{"nested", 1}}}});
        })));
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("many"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success(
                nlohmann::json::array({1, 2, 3}));
        })));
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("long"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success("12345");
        })));
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("nan"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success(
                std::numeric_limits<double>::quiet_NaN());
        })));
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("discarded"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success(
                nlohmann::json::parse("not-json", nullptr, false));
        })));
    ASSERT_TRUE(manager->freeze());

    const auto deep = manager->execute("deep", {});
    const auto many = manager->execute("many", {});
    const auto long_string = manager->execute("long", {});
    const auto nan = manager->execute("nan", {});
    const auto discarded = manager->execute("discarded", {});
    ASSERT_FALSE(deep);
    ASSERT_FALSE(many);
    ASSERT_FALSE(long_string);
    ASSERT_FALSE(nan);
    ASSERT_FALSE(discarded);
    EXPECT_EQ(deep.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(many.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(long_string.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(nan.error().code, ErrorCode::InternalError);
    EXPECT_EQ(discarded.error().code, ErrorCode::InternalError);
}

TEST(PluginManagerTest, RejectsMalformedOperationBeforeLookupAllocation) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->freeze());

    for (const std::string operation :
         {"", "Uppercase", "bad operation", ".leading"}) {
        const auto result = manager->find_metadata(operation);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(PluginManagerTest, BoundsFrameworkAndPluginFacingErrorMessages) {
    const auto limits = PluginLimits::create(
        1, 64, 64, 64, 128, 4, 4, 4, 8, 32, 32, 8, 32).value();
    const auto manager = make_manager(limits);
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("bounded"),
        FunctionPlugin::Validator{},
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success("abc");
        })));
    ASSERT_TRUE(manager->freeze());

    const auto unknown = manager->find_metadata("missing");
    const auto input = manager->validate("bounded", "abc");
    const auto output = manager->execute("bounded", {});
    ASSERT_FALSE(unknown);
    ASSERT_FALSE(input);
    ASSERT_FALSE(output);
    EXPECT_LE(unknown.error().message.size(), 4U);
    EXPECT_LE(input.error().message.size(), 4U);
    EXPECT_LE(output.error().message.size(), 4U);
}

TEST(PluginManagerTest, RegisterFreezeExecuteRaceUsesRegistryLinearization) {
    const auto manager = make_manager();
    const auto plugin = std::make_shared<FunctionPlugin>(
        metadata_for("racing"),
        [](const nlohmann::json&) {
            return Result<void>::success();
        },
        [](const nlohmann::json&) {
            return Result<nlohmann::json>::success({{"executed", true}});
        });
    std::promise<void> metadata_entered;
    auto entered = metadata_entered.get_future();
    std::promise<void> release_metadata;
    auto release = release_metadata.get_future().share();
    plugin->metadata_hook = [&metadata_entered, release] {
        metadata_entered.set_value();
        release.wait();
    };

    auto registration = std::async(
        std::launch::async,
        [manager, plugin] { return manager->register_plugin(plugin); });
    const auto entered_status = entered.wait_for(2s);
    if (entered_status != std::future_status::ready) {
        release_metadata.set_value();
        EXPECT_EQ(
            registration.wait_for(2s),
            std::future_status::ready);
        FAIL() << "metadata callback did not start before the deadline";
        return;
    }
    const auto configuring_execute = manager->execute("racing", {});
    ASSERT_FALSE(configuring_execute);
    EXPECT_EQ(
        configuring_execute.error().code,
        ErrorCode::InvalidState);
    EXPECT_EQ(plugin->validation_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(plugin->execution_calls.load(std::memory_order_relaxed), 0);

    ASSERT_TRUE(manager->freeze());
    const auto frozen_execute = manager->execute("racing", {});
    ASSERT_FALSE(frozen_execute);
    EXPECT_EQ(frozen_execute.error().code, ErrorCode::NotFound);
    release_metadata.set_value();
    ASSERT_EQ(registration.wait_for(2s), std::future_status::ready);
    const auto result = registration.get();

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidState);
    EXPECT_EQ(manager->size(), 0U);
    EXPECT_EQ(plugin->validation_calls.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(plugin->execution_calls.load(std::memory_order_relaxed), 0);
}

TEST(PluginManagerTest, FreezeLookupRaceHasOnlyDocumentedOutcomes) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("lookup"))));
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto freezing = std::async(
        std::launch::async,
        [manager, gate] {
            gate.wait();
            return manager->freeze();
        });
    auto lookup = std::async(
        std::launch::async,
        [manager, gate] {
            gate.wait();
            return manager->find_metadata("lookup");
        });
    release.set_value();

    ASSERT_EQ(freezing.wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(freezing.get());
    ASSERT_EQ(lookup.wait_for(2s), std::future_status::ready);
    const auto raced_lookup = lookup.get();
    if (!raced_lookup) {
        EXPECT_EQ(raced_lookup.error().code, ErrorCode::InvalidState);
    }
    EXPECT_TRUE(manager->find_metadata("lookup"));
}

TEST(PluginManagerTest, RegistryLockIsReleasedBeforePluginExecution) {
    const auto manager = make_manager();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    auto release_future = release.get_future().share();
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("blocking"),
        FunctionPlugin::Validator{},
        [&entered, release_future](const nlohmann::json&) {
            entered.set_value();
            release_future.wait();
            return Result<nlohmann::json>::success({{"done", true}});
        })));
    ASSERT_TRUE(manager->freeze());

    auto execution = std::async(
        std::launch::async,
        [&manager] { return manager->execute("blocking", {}); });
    const auto entered_status = entered_future.wait_for(2s);
    if (entered_status != std::future_status::ready) {
        release.set_value();
        execution.wait();
        FAIL() << "plugin execution did not start before the deadline";
        return;
    }
    auto lookup = std::async(
        std::launch::async,
        [&manager] { return manager->find_metadata("blocking"); });
    const auto lookup_status = lookup.wait_for(2s);
    if (lookup_status != std::future_status::ready) {
        release.set_value();
        EXPECT_EQ(execution.wait_for(2s), std::future_status::ready);
        FAIL() << "metadata lookup blocked behind plugin execution";
        return;
    }
    EXPECT_TRUE(lookup.get());
    release.set_value();
    ASSERT_EQ(execution.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(execution.get());
}

TEST(PluginManagerTest, PluginExecutionCanReenterMetadataLookup) {
    const auto manager = make_manager();
    std::weak_ptr<PluginManager> weak_manager = manager;
    ASSERT_TRUE(manager->register_plugin(std::make_shared<FunctionPlugin>(
        metadata_for("reentrant"),
        FunctionPlugin::Validator{},
        [weak_manager](const nlohmann::json&) {
            const auto locked = weak_manager.lock();
            if (!locked) {
                return Result<nlohmann::json>::failure(make_error(
                    ErrorCode::InternalError,
                    "manager expired"));
            }
            auto metadata = locked->find_metadata("reentrant");
            if (!metadata) {
                return Result<nlohmann::json>::failure(
                    std::move(metadata).error());
            }
            return Result<nlohmann::json>::success(
                {{"operation", metadata.value().operation}});
        })));
    ASSERT_TRUE(manager->freeze());

    const auto result = manager->execute("reentrant", {});

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().at("operation"), "reentrant");
}

TEST(PluginManagerTest, FrozenRegistrySupportsConcurrentReadValidateExecute) {
    const auto manager = make_manager();
    ASSERT_TRUE(manager->register_plugin(
        std::make_shared<FunctionPlugin>(metadata_for("parallel"))));
    ASSERT_TRUE(manager->freeze());
    std::vector<std::future<bool>> futures;
    for (int index = 0; index < 24; ++index) {
        futures.push_back(std::async(
            std::launch::async,
            [manager, index] {
                const auto metadata = manager->find_metadata("parallel");
                const auto valid =
                    manager->validate("parallel", {{"index", index}});
                const auto output =
                    manager->execute("parallel", {{"index", index}});
                return metadata && valid && output &&
                       output.value().at("index") == index;
            }));
    }

    for (auto& future : futures) {
        ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
        EXPECT_TRUE(future.get());
    }
}

}  // namespace
