#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

#include "iaisf/plugin/abi/plugin_abi_adapter.hpp"
#include "iaisf/plugin/abi/plugin_abi_validation.hpp"
#include "iaisf/plugin/plugin_runtime.hpp"

struct iaisf_plugin_handle {
    void* state{nullptr};
};

namespace iaisf::plugin::abi {
namespace {

struct FakeState;

struct FakeState {
    iaisf_plugin_handle handle{this};
    int create_calls{0};
    int validate_calls{0};
    int execute_calls{0};
    int shutdown_calls{0};
    int destroy_calls{0};
};

iaisf_plugin_string_view view(const char* text) {
    return iaisf_plugin_string_view{text, std::strlen(text)};
}

iaisf_plugin_status_t fake_get_metadata(
    void*,
    iaisf_plugin_metadata* metadata) {
    static const iaisf_plugin_string_view capabilities[] = {
        view("test")};
    if (metadata == nullptr || metadata->struct_size < sizeof(*metadata)) {
        return IAISF_PLUGIN_STATUS_STRUCT_TOO_SMALL;
    }
    metadata->abi_version = IAISF_PLUGIN_ABI_VERSION;
    metadata->struct_size = sizeof(*metadata);
    metadata->operation = view("abi.fake");
    metadata->name = view("ABI Fake");
    metadata->version = view("1.0.0");
    metadata->description = view("fake ABI plugin");
    metadata->flags = 0U;
    metadata->reserved = 0U;
    metadata->capabilities = capabilities;
    metadata->capability_count = 1U;
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t fake_create(
    void* context,
    const iaisf_plugin_host_api* host,
    iaisf_plugin_handle** instance) {
    if (context == nullptr || host == nullptr || instance == nullptr ||
        host->log == nullptr || host->allocate == nullptr ||
        host->deallocate == nullptr) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    auto* state = static_cast<FakeState*>(context);
    ++state->create_calls;
    *instance = &state->handle;
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t fake_validate(
    iaisf_plugin_handle* instance,
    iaisf_plugin_bytes_view input) {
    if (instance == nullptr || input.data == nullptr || input.size == 0U) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    ++static_cast<FakeState*>(instance->state)->validate_calls;
    return IAISF_PLUGIN_STATUS_OK;
}

iaisf_plugin_status_t fake_execute(
    iaisf_plugin_handle* instance,
    iaisf_plugin_bytes_view input,
    iaisf_plugin_output_fn output,
    void* output_context) {
    if (instance == nullptr || input.data == nullptr || input.size == 0U ||
        output == nullptr) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    ++static_cast<FakeState*>(instance->state)->execute_calls;
    static const char response[] = "{\"ok\":true}";
    const iaisf_plugin_bytes_view bytes{
        reinterpret_cast<const uint8_t*>(response), sizeof(response) - 1U};
    return output(output_context, bytes);
}

iaisf_plugin_status_t fake_shutdown(iaisf_plugin_handle* instance) {
    if (instance == nullptr) {
        return IAISF_PLUGIN_STATUS_INVALID_ARGUMENT;
    }
    ++static_cast<FakeState*>(instance->state)->shutdown_calls;
    return IAISF_PLUGIN_STATUS_OK;
}

void fake_destroy(iaisf_plugin_handle* instance) {
    if (instance != nullptr && instance->state != nullptr) {
        ++static_cast<FakeState*>(instance->state)->destroy_calls;
    }
}

iaisf_plugin_api make_api(FakeState& state) {
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
    return api;
}

TEST(PluginAbiTest, CppHeaderExposesPureCContract) {
    EXPECT_EQ(IAISF_PLUGIN_ABI_VERSION, 1U);
    EXPECT_EQ(sizeof(iaisf_plugin_string_view), sizeof(void*) * 2U);
}

TEST(PluginAbiTest, RejectsVersionMismatch) {
    FakeState state;
    auto api = make_api(state);
    api.abi_version = IAISF_PLUGIN_ABI_VERSION + 1U;
    auto result = validate_plugin_api(api);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PluginAbiTest, RejectsStructThatIsTooSmall) {
    FakeState state;
    auto api = make_api(state);
    api.struct_size = static_cast<uint32_t>(sizeof(uint32_t));
    auto result = validate_plugin_api(api);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PluginAbiTest, RejectsMissingRequiredFunction) {
    FakeState state;
    auto api = make_api(state);
    api.execute = nullptr;
    auto result = validate_plugin_api(api);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PluginAbiTest, RejectsInvalidHostCallbacks) {
    iaisf_plugin_host_api host{};
    host.abi_version = IAISF_PLUGIN_ABI_VERSION;
    host.struct_size = sizeof(host);
    host.log = nullptr;
    host.allocate = nullptr;
    host.deallocate = nullptr;
    auto result = validate_host_api(host);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PluginAbiTest, RejectsMetadataLimitViolation) {
    FakeState state;
    auto api = make_api(state);
    auto limits = PluginLimits::create(128, 4);
    ASSERT_TRUE(limits);
    iaisf_plugin_metadata metadata{};
    metadata.abi_version = IAISF_PLUGIN_ABI_VERSION;
    metadata.struct_size = sizeof(metadata);
    ASSERT_EQ(api.get_metadata(api.plugin_context, &metadata),
              IAISF_PLUGIN_STATUS_OK);
    auto result = validate_metadata_view(metadata, limits.value());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::InvalidArgument);
}

TEST(PluginAbiTest, AdapterBridgesFakePluginAndOwnsLifecycle) {
    FakeState state;
    auto limits = PluginLimits::create();
    ASSERT_TRUE(limits);
    auto adapter_result = AbiPluginAdapter::create(make_api(state), limits.value());
    ASSERT_TRUE(adapter_result);
    auto adapter = std::move(adapter_result).value();
    EXPECT_EQ(adapter->metadata().operation, "abi.fake");
    ASSERT_TRUE(adapter->validate_input(nlohmann::json{{"x", 1}}));
    auto executed = adapter->execute(nlohmann::json{{"x", 1}});
    ASSERT_TRUE(executed);
    EXPECT_TRUE(executed.value().at("ok").get<bool>());
    EXPECT_EQ(state.create_calls, 1);
    EXPECT_EQ(state.validate_calls, 1);
    EXPECT_EQ(state.execute_calls, 1);

    auto runtime_result = PluginRuntime::create(limits.value());
    ASSERT_TRUE(runtime_result);
    auto runtime = std::move(runtime_result).value();
    ASSERT_TRUE(runtime->register_plugin(adapter));
    ASSERT_TRUE(runtime->freeze());
    ASSERT_TRUE(runtime->shutdown());
    EXPECT_EQ(state.shutdown_calls, 1);
    runtime.reset();
    adapter.reset();
    EXPECT_EQ(state.destroy_calls, 1);
}

}  // namespace
}  // namespace iaisf::plugin::abi
