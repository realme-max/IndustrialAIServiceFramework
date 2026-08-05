#include <csignal>
#include <gtest/gtest.h>

#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/service/industrial_ai_service.hpp"
#include "iaisf/service/service_options.hpp"

namespace iaisf::service {
namespace {

class QuietLogger final : public ILogger {
public:
    void log(LogLevel, std::string_view, std::string_view) override {}
};

TEST(HealthLifecycleTest, CreatedAndExplicitStopReachStopped) {
    QuietLogger logger;
    auto loop = net::EventLoop::create(logger).value();
    auto service = IndustrialAiService::create(
        *loop,
        logger,
        net::tcp::Ipv4Endpoint::loopback(0U),
        ServiceOptions::defaults().value()).value();
    EXPECT_EQ(service->health_status().phase, health::HealthPhase::Created);
    EXPECT_TRUE(service->stop());
    EXPECT_EQ(service->health_status().phase, health::HealthPhase::Stopped);
    EXPECT_FALSE(service->health_status().live);
    EXPECT_FALSE(service->health_status().ready);
}

TEST(HealthLifecycleTest, SignalStopTransitionsRunningToStopped) {
    QuietLogger logger;
    auto loop = net::EventLoop::create(logger).value();
    auto service = IndustrialAiService::create(
        *loop,
        logger,
        net::tcp::Ipv4Endpoint::loopback(0U),
        ServiceOptions::defaults().value()).value();
    ASSERT_TRUE(service->start());
    EXPECT_EQ(service->health_status().phase, health::HealthPhase::Running);
    ASSERT_EQ(::raise(SIGTERM), 0);
    ASSERT_TRUE(loop->run());
    EXPECT_EQ(service->health_status().phase, health::HealthPhase::Stopped);
    EXPECT_FALSE(service->health_status().ready);
}

}  // namespace
}  // namespace iaisf::service
