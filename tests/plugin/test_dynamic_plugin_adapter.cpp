#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "iaisf/plugin/dynamic_plugin_adapter.hpp"
#include "iaisf/plugin/detail/dynamic_module.hpp"
#include "iaisf/plugin/detail/dynamic_plugin_loader.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/plugin/plugin_runtime.hpp"

namespace iaisf::plugin {
namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{IAISF_DYNAMIC_FIXTURE_PATH};
}

PluginMetadata fake_metadata(const std::string& operation = "fake_plugin") {
    return PluginMetadata{
        operation, "Fake plugin", "1.0", "ABI test plugin", false, {}};
}

struct FakeState {
    enum class Mode { Normal, InvalidJson, ThrowExecute, OversizedOutput };
    Mode mode{Mode::Normal};
    iaisf_plugin_status_t create_status{IAISF_PLUGIN_STATUS_OK};
    iaisf_plugin_status_t initialize_status{IAISF_PLUGIN_STATUS_OK};
    iaisf_plugin_status_t shutdown_status{IAISF_PLUGIN_STATUS_OK};
    bool shutdown_throws{false};
    bool destroy_throws{false};
    std::string output{"{\"ok\":true}"};
    std::size_t initialize_calls{0U};
    std::size_t shutdown_calls{0U};
    std::size_t destroy_calls{0U};
};

struct FakeInstance {
    FakeState* state{nullptr};
    const iaisf_plugin_host_api* host{nullptr};
};

iaisf_plugin_status_t IAISF_PLUGIN_CALL fake_get_metadata(
    void* const context,
    iaisf_plugin_metadata* const metadata) {
    (void)context;
    if (metadata == nullptr) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    metadata->abi_version = IAISF_PLUGIN_ABI_VERSION;
    metadata->struct_size = sizeof(*metadata);
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL fake_create(
    void* const context,
    const iaisf_plugin_host_api* const host,
    iaisf_plugin_handle** const instance) {
    if (context == nullptr || host == nullptr || instance == nullptr ||
        host->allocate == nullptr) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    auto* const state = static_cast<FakeState*>(context);
    if (state->create_status != IAISF_PLUGIN_STATUS_OK) {
        return state->create_status;
    }
    auto* const memory = host->allocate(host->context, sizeof(FakeInstance));
    if (memory == nullptr) {
        return IAISF_PLUGIN_STATUS_OUT_OF_MEMORY;
    }
    *instance = reinterpret_cast<iaisf_plugin_handle*>(
        new (memory) FakeInstance{state, host});
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL fake_validate(
    iaisf_plugin_handle* const instance,
    const iaisf_plugin_bytes_view input) {
    if (instance == nullptr || (input.size != 0U && input.data == nullptr)) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL fake_execute(
    iaisf_plugin_handle* const instance,
    const iaisf_plugin_bytes_view input,
    const iaisf_plugin_output_fn output,
    void* const output_context) {
    if (instance == nullptr || output == nullptr ||
        (input.size != 0U && input.data == nullptr)) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    auto* const value = reinterpret_cast<FakeInstance*>(instance);
    if (value->state->mode == FakeState::Mode::ThrowExecute) {
        throw std::runtime_error{"ABI exception"};
    }
    const std::string bytes =
        value->state->mode == FakeState::Mode::InvalidJson
            ? "not-json"
            : value->state->output;
    return output(
        output_context,
        iaisf_plugin_bytes_view{
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL fake_initialize(
    iaisf_plugin_handle* const instance,
    const iaisf_plugin_bytes_view config) {
    if (instance == nullptr || (config.size != 0U && config.data == nullptr)) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    auto* const value = reinterpret_cast<FakeInstance*>(instance);
    ++value->state->initialize_calls;
    return value->state->initialize_status;
}

iaisf_plugin_status_t IAISF_PLUGIN_CALL fake_shutdown(
    iaisf_plugin_handle* const instance) {
    if (instance == nullptr) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    auto* const value = reinterpret_cast<FakeInstance*>(instance);
    ++value->state->shutdown_calls;
    if (value->state->shutdown_throws) {
        throw std::runtime_error{"shutdown exception"};
    }
    return value->state->shutdown_status;
}

void IAISF_PLUGIN_CALL fake_destroy(iaisf_plugin_handle* const instance) {
    if (instance == nullptr) {
        return;
    }
    auto* const value = reinterpret_cast<FakeInstance*>(instance);
    ++value->state->destroy_calls;
    const auto* const host = value->host;
    const bool should_throw = value->state->destroy_throws;
    value->~FakeInstance();
    host->deallocate(host->context, value);
    if (should_throw) {
        throw std::runtime_error{"destroy exception"};
    }
}

iaisf_plugin_api fake_api(FakeState& state, const bool initialize = true) {
    iaisf_plugin_api api{};
    api.abi_version = IAISF_PLUGIN_ABI_VERSION;
    api.struct_size = sizeof(api);
    api.plugin_context = &state;
    api.get_metadata = &fake_get_metadata;
    api.create = &fake_create;
    api.validate = &fake_validate;
    api.execute = &fake_execute;
    api.shutdown = &fake_shutdown;
    api.destroy = &fake_destroy;
    api.initialize = initialize ? &fake_initialize : nullptr;
    if (!initialize) {
        api.struct_size = static_cast<uint32_t>(
            offsetof(iaisf_plugin_api, initialize));
    }
    return api;
}

Result<std::shared_ptr<detail::DynamicModule>> module_for_fixture() {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    if (!loader) {
        return Result<std::shared_ptr<detail::DynamicModule>>::failure(
            std::move(loader).error());
    }
    return std::move(loader).value()->load_module(
        detail::DynamicPluginModuleSpec{"fake", fixture_path().filename()});
}

TEST(DynamicPluginAdapterTest, LoadsFixtureCreatesInitializesAndExecutes) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    EXPECT_EQ(adapter.value()->state(), DynamicPluginAdapterState::Created);
    EXPECT_EQ(adapter.value()->metadata().operation, "dynamic_fixture");
    ASSERT_TRUE(adapter.value()->initialize());
    EXPECT_EQ(adapter.value()->state(), DynamicPluginAdapterState::Initialized);
    ASSERT_TRUE(adapter.value()->validate_input(nlohmann::json::object()));
    auto output = adapter.value()->execute(nlohmann::json::object());
    ASSERT_TRUE(output);
    EXPECT_TRUE(output.value().at("fixture"));
    ASSERT_TRUE(adapter.value()->shutdown());
    EXPECT_EQ(adapter.value()->state(), DynamicPluginAdapterState::Stopped);
}

TEST(DynamicPluginAdapterTest, InvalidJsonOutputFailsClosed) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    state.mode = FakeState::Mode::InvalidJson;
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state), fake_metadata());
    ASSERT_TRUE(adapter);
    ASSERT_TRUE(adapter.value()->initialize());
    auto result = adapter.value()->execute(nlohmann::json::object());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InternalError);
}

TEST(DynamicPluginAdapterTest, CreateFailureDoesNotPublishAnInstance) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    state.create_status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state), fake_metadata());
    ASSERT_FALSE(adapter);
    EXPECT_EQ(adapter.error().code, ErrorCode::InternalError);
    EXPECT_EQ(state.destroy_calls, 0U);
}

TEST(DynamicPluginAdapterTest, OversizedOutputIsRejected) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    state.output = "{\"long\":true}";
    auto limits = PluginLimits::create(
        8, 128, 128, 64, 1024, 1024, 1024, 8, 64, 1000, 1024, 64, 128);
    ASSERT_TRUE(limits);
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state), fake_metadata(),
        std::move(limits).value());
    ASSERT_TRUE(adapter);
    ASSERT_TRUE(adapter.value()->initialize());
    auto result = adapter.value()->execute(nlohmann::json::object());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
}

TEST(DynamicPluginAdapterTest, InitializeFailureDestroysWithoutShutdown) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    state.initialize_status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state), fake_metadata(),
        nlohmann::json{{"configured", true}});
    ASSERT_TRUE(adapter);
    auto result = adapter.value()->initialize();
    ASSERT_FALSE(result);
    EXPECT_EQ(adapter.value()->state(), DynamicPluginAdapterState::Failed);
    EXPECT_EQ(state.shutdown_calls, 0U);
    EXPECT_EQ(state.destroy_calls, 1U);
}

TEST(DynamicPluginAdapterTest, ShutdownFailureStillDestroysExactlyOnce) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    state.shutdown_status = IAISF_PLUGIN_STATUS_SHUTDOWN_FAILED;
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state), fake_metadata());
    ASSERT_TRUE(adapter);
    ASSERT_TRUE(adapter.value()->initialize());
    auto result = adapter.value()->shutdown();
    ASSERT_FALSE(result);
    EXPECT_EQ(state.shutdown_calls, 1U);
    EXPECT_EQ(state.destroy_calls, 1U);
    EXPECT_EQ(adapter.value()->state(), DynamicPluginAdapterState::Stopped);
    EXPECT_FALSE(adapter.value()->shutdown());
    EXPECT_EQ(state.destroy_calls, 1U);
}

TEST(DynamicPluginAdapterTest, ShutdownAndDestroyExceptionsAreIsolated) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    state.shutdown_throws = true;
    state.destroy_throws = true;
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state), fake_metadata());
    ASSERT_TRUE(adapter);
    ASSERT_TRUE(adapter.value()->initialize());
    auto result = adapter.value()->shutdown();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InternalError);
    EXPECT_EQ(state.shutdown_calls, 1U);
    EXPECT_EQ(state.destroy_calls, 1U);
    EXPECT_EQ(adapter.value()->state(), DynamicPluginAdapterState::Stopped);
}

TEST(DynamicPluginAdapterTest, MissingInitializeRejectsNonEmptyConfig) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state, false), fake_metadata(),
        nlohmann::json{{"configured", true}});
    ASSERT_FALSE(adapter);
    EXPECT_EQ(adapter.error().code, ErrorCode::InvalidState);
}

TEST(DynamicPluginAdapterTest, AbiExceptionIsIsolated) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    state.mode = FakeState::Mode::ThrowExecute;
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state), fake_metadata());
    ASSERT_TRUE(adapter);
    ASSERT_TRUE(adapter.value()->initialize());
    auto result = adapter.value()->execute(nlohmann::json::object());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InternalError);
}

TEST(DynamicPluginAdapterTest, DynamicRegistrationIsTransactionalAndObservable) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    auto limits = PluginLimits::create();
    ASSERT_TRUE(limits);
    auto runtime = PluginRuntime::create(std::move(limits).value());
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->register_dynamic(
        std::move(adapter).value(),
        DynamicPluginRegistrationOptions{"fixture-module"}));
    auto snapshot = runtime.value()->entry_snapshot("dynamic_fixture");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().origin, "dynamic");
    EXPECT_EQ(snapshot.value().module_id, "fixture-module");
    ASSERT_TRUE(runtime.value()->freeze());
    ASSERT_TRUE(runtime.value()->shutdown());
}

TEST(DynamicPluginAdapterTest, DynamicCreationMetricsAreLabelFreeAndBestEffort) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    MetricsRegistry metrics;
    auto runtime = PluginRuntime::create(
        PluginLimits::create().value(), &metrics);
    ASSERT_TRUE(runtime);
    auto dynamic = std::move(adapter).value();
    ASSERT_TRUE(runtime.value()->register_dynamic(
        dynamic, DynamicPluginRegistrationOptions{"metrics"}));
    ASSERT_FALSE(runtime.value()->register_dynamic(
        dynamic, DynamicPluginRegistrationOptions{"duplicate"}));
    auto created = metrics.get_counter(
        "plugin_dynamic_plugin_creations_total");
    auto failed = metrics.get_counter(
        "plugin_dynamic_plugin_creation_failures_total");
    ASSERT_TRUE(created);
    ASSERT_TRUE(failed);
    EXPECT_EQ(created.value()->snapshot(), 1U);
    EXPECT_EQ(failed.value()->snapshot(), 1U);
}

TEST(DynamicPluginAdapterTest, DynamicUnloadMetricIsRegistered) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    MetricsRegistry metrics;
    auto runtime = PluginRuntime::create(
        PluginLimits::create().value(), &metrics);
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->register_dynamic(
        std::move(adapter).value(), DynamicPluginRegistrationOptions{"unload"}));
    ASSERT_TRUE(runtime.value()->freeze());
    ASSERT_TRUE(runtime.value()->shutdown());

    auto metric = metrics.get_counter("plugin_dynamic_unload_failures_total");
    ASSERT_TRUE(metric);
    EXPECT_EQ(metric.value()->snapshot(), 0U);
}

TEST(DynamicPluginAdapterTest, MetricsUnavailableDoesNotAffectExecution) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    auto runtime = PluginRuntime::create(PluginLimits::create().value());
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->register_dynamic(
        std::move(adapter).value(), DynamicPluginRegistrationOptions{"no-metrics"}));
    ASSERT_TRUE(runtime.value()->freeze());
    auto result = runtime.value()->execute(
        "dynamic_fixture", nlohmann::json::object());
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.value().at("fixture"));
    ASSERT_TRUE(runtime.value()->shutdown());
}

TEST(DynamicPluginAdapterTest, MetricTypeFailureIsBestEffort) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    MetricsRegistry metrics;
    ASSERT_TRUE(metrics.create_gauge("plugin_dynamic_unload_failures_total"));
    auto runtime = PluginRuntime::create(
        PluginLimits::create().value(), &metrics);
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->register_dynamic(
        std::move(adapter).value(), DynamicPluginRegistrationOptions{"bad-metric"}));
    ASSERT_TRUE(runtime.value()->freeze());
    auto result = runtime.value()->execute(
        "dynamic_fixture", nlohmann::json::object());
    ASSERT_TRUE(result);
    ASSERT_TRUE(runtime.value()->shutdown());
}

TEST(DynamicPluginAdapterTest, DuplicateOperationLeavesNoDynamicEntry) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    auto runtime = PluginRuntime::create(PluginLimits::create().value());
    ASSERT_TRUE(runtime);
    auto first = std::move(adapter).value();
    ASSERT_TRUE(runtime.value()->register_dynamic(
        first, DynamicPluginRegistrationOptions{"one"}));
    auto duplicate = runtime.value()->register_dynamic(
        std::move(first), DynamicPluginRegistrationOptions{"two"});
    ASSERT_FALSE(duplicate);
    EXPECT_FALSE(runtime.value()->entry_snapshot("not_present"));
}

TEST(DynamicPluginAdapterTest, CapacityFailureLeavesNoDynamicEntry) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    auto limits = PluginLimits::create(
        1, 128, 128, 64, 1024, 1024, 1024 * 1024, 4 * 1024 * 1024,
        64, 100000, 1024 * 1024, 64, 128);
    ASSERT_TRUE(limits);
    auto runtime = PluginRuntime::create(std::move(limits).value());
    ASSERT_TRUE(runtime);
    ASSERT_TRUE(runtime.value()->register_plugin(
        std::make_shared<EchoPlugin>()));
    auto result = runtime.value()->register_dynamic(
        std::move(adapter).value(), DynamicPluginRegistrationOptions{"full"});
    ASSERT_FALSE(result);
    EXPECT_FALSE(runtime.value()->entry_snapshot("dynamic_fixture"));
}

TEST(DynamicPluginAdapterTest, InitializeFailureRollsBackRuntimeEntry) {
    auto module = module_for_fixture();
    ASSERT_TRUE(module);
    FakeState state;
    state.initialize_status = IAISF_PLUGIN_STATUS_INTERNAL_ERROR;
    auto adapter = DynamicPluginAdapter::create(
        std::move(module).value(), fake_api(state), fake_metadata(),
        nlohmann::json{{"configured", true}});
    ASSERT_TRUE(adapter);
    auto runtime = PluginRuntime::create(PluginLimits::create().value());
    ASSERT_TRUE(runtime);
    auto result = runtime.value()->register_dynamic(
        std::move(adapter).value(), DynamicPluginRegistrationOptions{"failed"});
    ASSERT_FALSE(result);
    EXPECT_FALSE(runtime.value()->entry_snapshot("fake_plugin"));
    EXPECT_EQ(state.destroy_calls, 1U);
}

TEST(DynamicPluginAdapterTest, ModuleLifetimeExtendsThroughExecutionLease) {
    auto loader = detail::DynamicPluginLoader::create(
        detail::DynamicPluginLoaderOptions{fixture_path().parent_path()});
    ASSERT_TRUE(loader);
    auto adapter = std::move(loader).value()->load_plugin(
        detail::DynamicPluginModuleSpec{"fixture", fixture_path().filename()});
    ASSERT_TRUE(adapter);
    auto runtime = PluginRuntime::create(PluginLimits::create().value());
    ASSERT_TRUE(runtime);
    auto dynamic = std::move(adapter).value();
    ASSERT_TRUE(runtime.value()->register_dynamic(
        dynamic, DynamicPluginRegistrationOptions{"lease"}));
    ASSERT_TRUE(runtime.value()->freeze());
    auto lease = runtime.value()->acquire_execution_lease("dynamic_fixture");
    ASSERT_TRUE(lease);
    dynamic.reset();
    ASSERT_TRUE(lease.value().plugin()->execute(nlohmann::json::object()));
}

}  // namespace
}  // namespace iaisf::plugin
