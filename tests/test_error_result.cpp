#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "iaisf/core/error.hpp"
#include "iaisf/core/result.hpp"
#include "iaisf/version.hpp"

namespace {

using IntResult = iaisf::Result<int>;
using VoidResult = iaisf::Result<void>;

struct NonDefaultConstructible {
    NonDefaultConstructible() = delete;
    explicit NonDefaultConstructible(const int initial_value) : value(initial_value) {}

    int value;
};

static_assert(std::is_same_v<decltype(std::declval<IntResult&>().value()), int&>);
static_assert(
    std::is_same_v<decltype(std::declval<const IntResult&>().value()), const int&>);
static_assert(std::is_same_v<decltype(std::declval<IntResult&&>().value()), int&&>);
static_assert(
    std::is_same_v<decltype(std::declval<IntResult&>().error()), iaisf::Error&>);
static_assert(
    std::is_same_v<
        decltype(std::declval<const IntResult&>().error()),
        const iaisf::Error&>);
static_assert(
    std::is_same_v<decltype(std::declval<IntResult&&>().error()), iaisf::Error&&>);
static_assert(
    std::is_same_v<decltype(std::declval<VoidResult&>().error()), iaisf::Error&>);
static_assert(
    std::is_same_v<
        decltype(std::declval<const VoidResult&>().error()),
        const iaisf::Error&>);
static_assert(
    std::is_same_v<decltype(std::declval<VoidResult&&>().error()), iaisf::Error&&>);

TEST(VersionTest, ExposesStableConfiguredVersion) {
    EXPECT_EQ(IAISF_VERSION_MAJOR, 0);
    EXPECT_EQ(IAISF_VERSION_MINOR, 1);
    EXPECT_EQ(IAISF_VERSION_PATCH, 0);
    EXPECT_EQ(std::string{IAISF_VERSION_STRING}, "0.1.0");
    EXPECT_EQ(iaisf::kVersionString, "0.1.0");
}

TEST(ErrorTest, ConvertsKnownAndUnknownCodesToStableStrings) {
    EXPECT_EQ(iaisf::to_string(iaisf::ErrorCode::InvalidArgument), "invalid_argument");
    EXPECT_EQ(iaisf::to_string(iaisf::ErrorCode::ConfigError), "config_error");
    EXPECT_EQ(iaisf::to_string(iaisf::ErrorCode::IoError), "io_error");
    EXPECT_EQ(iaisf::to_string(iaisf::ErrorCode::SystemError), "system_error");
    EXPECT_EQ(iaisf::to_string(iaisf::ErrorCode::InvalidState), "invalid_state");
    EXPECT_EQ(
        iaisf::to_string(iaisf::ErrorCode::ResourceExhausted),
        "resource_exhausted");
    EXPECT_EQ(iaisf::to_string(iaisf::ErrorCode::NotFound), "not_found");
    EXPECT_EQ(iaisf::to_string(iaisf::ErrorCode::InternalError), "internal_error");
    EXPECT_EQ(
        iaisf::to_string(static_cast<iaisf::ErrorCode>(999)),
        "unknown_error");
}

TEST(ErrorTest, NormalizesEmptyMessagesAtCreationAndResultBoundaries) {
    const iaisf::Error constructed{iaisf::ErrorCode::InternalError, ""};
    const iaisf::Error created =
        iaisf::make_error(iaisf::ErrorCode::InternalError, "");
    EXPECT_EQ(constructed.message, "unspecified error");
    EXPECT_EQ(created.message, "unspecified error");

    iaisf::Error mutated =
        iaisf::make_error(iaisf::ErrorCode::InternalError, "initial message");
    mutated.message.clear();
    auto failure = iaisf::Result<int>::failure(std::move(mutated));
    EXPECT_EQ(failure.error().message, "unspecified error");
}

TEST(ResultTest, StoresIntegerAndStringValues) {
    auto integer_result = iaisf::Result<int>::success(42);
    auto string_result = iaisf::Result<std::string>::success("ready");
    auto non_default_result =
        iaisf::Result<NonDefaultConstructible>::success(NonDefaultConstructible{73});

    ASSERT_TRUE(integer_result.has_value());
    ASSERT_TRUE(static_cast<bool>(string_result));
    ASSERT_TRUE(non_default_result.has_value());
    EXPECT_EQ(integer_result.value(), 42);
    EXPECT_EQ(string_result.value(), "ready");
    EXPECT_EQ(non_default_result.value().value, 73);
}

TEST(ResultTest, StoresAnError) {
    auto result = iaisf::Result<int>::failure(
        iaisf::make_error(iaisf::ErrorCode::InvalidArgument, "bad value"));

    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(static_cast<bool>(result));
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(result.error().message, "bad value");
}

TEST(ResultTest, SupportsVoidSuccessAndFailure) {
    auto success = iaisf::Result<void>::success();
    auto failure = iaisf::Result<void>::failure(
        iaisf::make_error(iaisf::ErrorCode::IoError, "read failed"));

    EXPECT_TRUE(success.has_value());
    EXPECT_NO_THROW(success.value());
    EXPECT_FALSE(failure.has_value());
    EXPECT_EQ(failure.error().code, iaisf::ErrorCode::IoError);
}

TEST(ResultTest, ThrowsLogicErrorOnApiMisuse) {
    auto success = iaisf::Result<int>::success(7);
    auto failure = iaisf::Result<int>::failure(
        iaisf::make_error(iaisf::ErrorCode::InternalError, "failed"));
    auto void_success = iaisf::Result<void>::success();
    auto void_failure = iaisf::Result<void>::failure(
        iaisf::make_error(iaisf::ErrorCode::InternalError, "failed"));

    EXPECT_THROW(static_cast<void>(failure.value()), std::logic_error);
    EXPECT_THROW(static_cast<void>(success.error()), std::logic_error);
    EXPECT_THROW(static_cast<void>(void_success.error()), std::logic_error);
    EXPECT_THROW(void_failure.value(), std::logic_error);
}

TEST(ResultTest, SupportsMoveOnlyValues) {
    auto result =
        iaisf::Result<std::unique_ptr<int>>::success(std::make_unique<int>(91));

    ASSERT_TRUE(result);
    ASSERT_NE(result.value(), nullptr);
    EXPECT_EQ(*result.value(), 91);

    std::unique_ptr<int> moved_value = std::move(result).value();
    ASSERT_NE(moved_value, nullptr);
    EXPECT_EQ(*moved_value, 91);
}

TEST(ResultTest, SupportsConstAndRvalueAccess) {
    const auto const_result = iaisf::Result<std::string>::success("constant");
    EXPECT_EQ(const_result.value(), "constant");

    auto movable_result = iaisf::Result<std::string>::success("movable");
    std::string moved = std::move(movable_result).value();
    EXPECT_EQ(moved, "movable");

    const auto const_failure = iaisf::Result<std::string>::failure(
        iaisf::make_error(iaisf::ErrorCode::IoError, "constant error"));
    EXPECT_EQ(const_failure.error().message, "constant error");

    auto movable_failure = iaisf::Result<std::string>::failure(
        iaisf::make_error(iaisf::ErrorCode::IoError, "movable error"));
    iaisf::Error moved_error = std::move(movable_failure).error();
    EXPECT_EQ(moved_error.message, "movable error");
}

}  // namespace
