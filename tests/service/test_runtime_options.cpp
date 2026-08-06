#include <chrono>
#include <utility>

#include <gtest/gtest.h>

#include "iaisf/config/app_config.hpp"
#include "iaisf/core/error.hpp"
#include "iaisf/service/runtime_options.hpp"
#include "iaisf/service/service_options.hpp"

namespace {

using iaisf::default_app_config;
using iaisf::ErrorCode;
using iaisf::service::make_runtime_options;
using iaisf::service::ServiceOptions;

TEST(RuntimeOptionsTest, MapsPortableConfigurationToValidatedRuntimeOptions) {
    auto config = default_app_config();
    config.server.host = "127.0.0.1";
    config.server.port = 0U;
    config.server.reactor.max_events = 512U;
    config.server.reactor.pending_callback_capacity = 2048U;
    config.server.reactor.max_timers = 4096U;
    config.server.tcp.max_connections = 100;
    config.server.tcp.idle_timeout_ms = 30000;
    config.http.header_timeout_ms = 5000;
    config.http.body_timeout_ms = 10000;
    config.plugins.enable_echo = false;

    const auto result = make_runtime_options(config);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().reactor_max_events(), 512U);
    EXPECT_EQ(result.value().pending_callback_capacity(), 2048U);
    EXPECT_EQ(result.value().timer_options().max_timers, 4096U);
    EXPECT_EQ(result.value().bind_endpoint().port(), 0U);
    EXPECT_EQ(result.value().service_options().tcp_options().idle_timeout(),
              std::chrono::milliseconds{30000});
    EXPECT_EQ(result.value().service_options().http_header_timeout(),
              std::chrono::milliseconds{5000});
    EXPECT_EQ(result.value().service_options().http_body_timeout(),
              std::chrono::milliseconds{10000});
    EXPECT_FALSE(result.value().service_options().enable_echo());
}

TEST(RuntimeOptionsTest, RejectsTimerCapacityBelowEnabledLayerDemand) {
    auto config = default_app_config();
    config.server.tcp.max_connections = 100;
    config.server.tcp.idle_timeout_ms = 1000;
    config.http.header_timeout_ms = 1000;
    config.server.reactor.max_timers = 199U;

    const auto result = make_runtime_options(config);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ConfigError);
}

TEST(RuntimeOptionsTest, CountsHeaderAndBodyAsOneMutuallyExclusiveLayer) {
    auto config = default_app_config();
    config.server.tcp.max_connections = 100;
    config.http.header_timeout_ms = 1000;
    config.http.body_timeout_ms = 2000;
    config.server.reactor.max_timers = 100U;

    EXPECT_TRUE(make_runtime_options(config));
}

TEST(RuntimeOptionsTest, ConvertsComponentValidationFailuresToConfigError) {
    auto invalid_host = default_app_config();
    invalid_host.server.host = "localhost";
    auto host_result = make_runtime_options(invalid_host);
    ASSERT_FALSE(host_result);
    EXPECT_EQ(host_result.error().code, ErrorCode::ConfigError);

    auto invalid_buffers = default_app_config();
    invalid_buffers.server.tcp.input_initial_capacity_bytes =
        invalid_buffers.server.tcp.input_maximum_capacity_bytes + 1;
    auto buffer_result = make_runtime_options(invalid_buffers);
    ASSERT_FALSE(buffer_result);
    EXPECT_EQ(buffer_result.error().code, ErrorCode::ConfigError);
}

TEST(RuntimeOptionsTest, ServiceDefaultsUseConfigurationWorkerDefault) {
    const auto config = default_app_config();
    const auto service = ServiceOptions::defaults();
    ASSERT_TRUE(service);
    EXPECT_EQ(service.value().pool_options().worker_threads,
              config.runtime.worker_threads);
    EXPECT_EQ(service.value().pool_options().queue_capacity,
              config.runtime.task_queue_capacity);
}

TEST(RuntimeOptionsTest, RejectsMissingCurrentPlatformLibrary) {
    auto config = default_app_config();
    config.plugins.runtime.dynamic_loading_enabled = true;
    iaisf::DynamicPluginModuleConfig module;
    module.id = "platform";
#if defined(_WIN32)
    module.linux_library = "fixture.so";
#else
    module.windows_library = "fixture.dll";
#endif
    config.plugins.runtime.modules.push_back(std::move(module));
    const auto result = make_runtime_options(config);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::ConfigError);
}

} // namespace
