#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "iaisf/plugin/abi/plugin_abi.h"
#include "iaisf/plugin/dynamic_plugin_adapter.hpp"
#include "iaisf/plugin/detail/dynamic_module.hpp"
#include "iaisf/plugin/detail/dynamic_plugin_loader.hpp"
#include "iaisf/plugin/plugin_limits.hpp"

namespace iaisf::plugin::detail {
namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{IAISF_TEST_FIXTURE_PATH};
}

TEST(DynamicModuleTest, InvalidLibraryPathReturnsSystemError) {
    const auto result = DynamicModule::open(
        fixture_path().parent_path() / "does-not-exist.so");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::SystemError);
}

TEST(DynamicModuleTest, LoadsFixtureAndResolvesExportedSymbol) {
    auto result = DynamicModule::open(fixture_path());
    ASSERT_TRUE(result);
    auto module = std::move(result).value();
    ASSERT_TRUE(module.loaded());
    auto symbol = module.resolve_symbol("iaisf_test_symbol");
    ASSERT_TRUE(symbol);
    EXPECT_NE(symbol.value(), nullptr);
}

TEST(DynamicModuleTest, MissingSymbolReturnsNotFound) {
    auto result = DynamicModule::open(fixture_path());
    ASSERT_TRUE(result);
    auto symbol = result.value().resolve_symbol("iaisf_missing_symbol");
    ASSERT_FALSE(symbol);
    EXPECT_EQ(symbol.error().code, ErrorCode::NotFound);
}

TEST(DynamicModuleTest, MoveConstructorTransfersOwnership) {
    auto result = DynamicModule::open(fixture_path());
    ASSERT_TRUE(result);
    DynamicModule moved{std::move(result).value()};
    EXPECT_TRUE(moved.loaded());
    EXPECT_TRUE(moved.resolve_symbol("iaisf_test_symbol"));
}

TEST(DynamicModuleTest, MoveAssignmentReleasesPreviousHandle) {
    auto source_result = DynamicModule::open(fixture_path());
    auto target_result = DynamicModule::open(fixture_path());
    ASSERT_TRUE(source_result);
    ASSERT_TRUE(target_result);
    DynamicModule source{std::move(source_result).value()};
    DynamicModule target{std::move(target_result).value()};
    target = std::move(source);
    EXPECT_TRUE(target.loaded());
    EXPECT_TRUE(target.resolve_symbol("iaisf_test_symbol"));
}

TEST(DynamicPluginLoaderTest, CreatesAndLoadsValidFixture) {
    const auto fixture = fixture_path();
    auto loader_result = DynamicPluginLoader::create(
        DynamicPluginLoaderOptions{fixture.parent_path()});
    ASSERT_TRUE(loader_result);
    auto loader = std::move(loader_result).value();
    auto module = loader->load_module(DynamicPluginModuleSpec{
        "fixture",
        fixture.filename()});
    ASSERT_TRUE(module);
    EXPECT_TRUE(module.value()->loaded());
    EXPECT_TRUE(module.value()->resolve_symbol("iaisf_test_symbol"));
}

TEST(DynamicPluginLoaderTest, RejectsInvalidRootAndModulePath) {
    const auto fixture = fixture_path();
    auto invalid_root = DynamicPluginLoader::create(
        DynamicPluginLoaderOptions{fixture.parent_path() / "missing-root"});
    ASSERT_FALSE(invalid_root);
    EXPECT_EQ(invalid_root.error().code, ErrorCode::NotFound);

    auto loader_result = DynamicPluginLoader::create(
        DynamicPluginLoaderOptions{fixture.parent_path()});
    ASSERT_TRUE(loader_result);
    auto loader = std::move(loader_result).value();
    auto invalid_module = loader->load_module(DynamicPluginModuleSpec{
        "invalid",
        std::filesystem::path{"../"} / fixture.filename()});
    ASSERT_FALSE(invalid_module);
    EXPECT_EQ(invalid_module.error().code, ErrorCode::InvalidArgument);
}

TEST(DynamicPluginLoaderTest, RejectsDuplicateNormalizedPath) {
    const auto fixture = fixture_path();
    auto loader_result = DynamicPluginLoader::create(
        DynamicPluginLoaderOptions{fixture.parent_path()});
    ASSERT_TRUE(loader_result);
    auto loader = std::move(loader_result).value();
    const DynamicPluginModuleSpec spec{"fixture", fixture.filename()};
    ASSERT_TRUE(loader->load_module(spec));
    auto duplicate = loader->load_module(spec);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::InvalidState);
}

TEST(DynamicPluginLoaderTest, FailedAdapterCreationRollsBackPathReservation) {
    const auto fixture = std::filesystem::path{IAISF_DYNAMIC_FIXTURE_PATH};
    auto loader_result = DynamicPluginLoader::create(
        DynamicPluginLoaderOptions{fixture.parent_path()});
    ASSERT_TRUE(loader_result);
    auto loader = std::move(loader_result).value();

    auto restrictive_limits = PluginLimits::create(
        128, 128, 128, 64, 1024, 1024, 1, 4 * 1024 * 1024, 64,
        100000, 1024 * 1024, 64, 128);
    ASSERT_TRUE(restrictive_limits);
    const DynamicPluginModuleSpec spec{
        "retry", fixture.filename(), "{\"configured\":true}"};
    auto failed = loader->load_plugin(
        spec, std::move(restrictive_limits).value());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::ResourceExhausted);

    auto normal_limits = PluginLimits::create();
    ASSERT_TRUE(normal_limits);
    auto retried = loader->load_plugin(
        spec, std::move(normal_limits).value());
    ASSERT_TRUE(retried);
    ASSERT_TRUE(retried.value()->initialize());
    ASSERT_TRUE(retried.value()->shutdown());
}

}  // namespace
}  // namespace iaisf::plugin::detail
