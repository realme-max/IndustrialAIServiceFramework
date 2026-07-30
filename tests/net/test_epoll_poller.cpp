#include <cerrno>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "iaisf/core/error.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/channel.hpp"
#include "iaisf/net/epoll_poller.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace {

class NullLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        std::string_view) override {}
};

std::unique_ptr<iaisf::net::EventLoop> make_loop(NullLogger& logger) {
    auto result = iaisf::net::EventLoop::create(logger, 16U, 16U);
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

std::unique_ptr<iaisf::net::EpollPoller> make_poller() {
    auto result = iaisf::net::EpollPoller::create(8U);
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

iaisf::net::UniqueFd make_event_fd() {
    return iaisf::net::UniqueFd{::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC)};
}

void signal_eventfd(const int descriptor, const std::uint64_t value = 1U) {
    ASSERT_EQ(
        ::write(descriptor, &value, sizeof(value)),
        static_cast<ssize_t>(sizeof(value)));
}

void drain_eventfd(const int descriptor) {
    std::uint64_t value = 0U;
    ASSERT_EQ(
        ::read(descriptor, &value, sizeof(value)),
        static_cast<ssize_t>(sizeof(value)));
}

TEST(EpollPollerTest, RejectsInvalidMaximumEventCounts) {
    auto zero = iaisf::net::EpollPoller::create(0U);
    auto excessive =
        iaisf::net::EpollPoller::create(iaisf::net::EpollPoller::kMaximumEvents + 1U);

    ASSERT_FALSE(zero);
    ASSERT_FALSE(excessive);
    EXPECT_EQ(zero.error().code, iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(excessive.error().code, iaisf::ErrorCode::InvalidArgument);
}

TEST(EpollPollerTest, AddPollUpdateAndRemoveChannel) {
    NullLogger logger;
    auto loop = make_loop(logger);
    auto poller = make_poller();
    auto event_fd = make_event_fd();
    ASSERT_NE(loop, nullptr);
    ASSERT_NE(poller, nullptr);
    ASSERT_TRUE(event_fd.valid());
    iaisf::net::Channel channel{*loop, event_fd.get()};
    channel.enable_reading();

    ASSERT_TRUE(poller->add(channel));
    EXPECT_TRUE(poller->contains(channel));
    EXPECT_EQ(poller->registered_count(), 1U);

    signal_eventfd(event_fd.get());
    std::vector<iaisf::net::Channel*> active;
    auto poll_result = poller->poll(100, active);
    ASSERT_TRUE(poll_result);
    ASSERT_EQ(poll_result.value(), 1U);
    ASSERT_EQ(active.size(), 1U);
    EXPECT_EQ(active.front(), &channel);
    EXPECT_NE(
        channel.ready_events() & iaisf::net::Channel::kReadEvents,
        0U);
    drain_eventfd(event_fd.get());

    channel.enable_writing();
    ASSERT_TRUE(poller->update(channel));
    ASSERT_TRUE(poller->remove(channel));
    EXPECT_FALSE(poller->contains(channel));
    EXPECT_FALSE(channel.is_registered());
}

TEST(EpollPollerTest, RejectsDuplicateAndUnknownOperations) {
    NullLogger logger;
    auto loop = make_loop(logger);
    auto poller = make_poller();
    auto event_fd = make_event_fd();
    ASSERT_NE(loop, nullptr);
    ASSERT_NE(poller, nullptr);
    ASSERT_TRUE(event_fd.valid());
    iaisf::net::Channel channel{*loop, event_fd.get()};
    channel.enable_reading();

    ASSERT_TRUE(poller->add(channel));
    auto duplicate = poller->add(channel);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, iaisf::ErrorCode::InvalidState);
    ASSERT_TRUE(poller->remove(channel));

    auto update_missing = poller->update(channel);
    auto remove_missing = poller->remove(channel);
    ASSERT_FALSE(update_missing);
    ASSERT_FALSE(remove_missing);
    EXPECT_EQ(update_missing.error().code, iaisf::ErrorCode::InvalidState);
    EXPECT_EQ(remove_missing.error().code, iaisf::ErrorCode::InvalidState);
}

TEST(EpollPollerTest, RemovedDescriptorIsNoLongerDelivered) {
    NullLogger logger;
    auto loop = make_loop(logger);
    auto poller = make_poller();
    auto event_fd = make_event_fd();
    ASSERT_NE(loop, nullptr);
    ASSERT_NE(poller, nullptr);
    ASSERT_TRUE(event_fd.valid());
    iaisf::net::Channel channel{*loop, event_fd.get()};
    channel.enable_reading();
    ASSERT_TRUE(poller->add(channel));
    ASSERT_TRUE(poller->remove(channel));

    signal_eventfd(event_fd.get());
    std::vector<iaisf::net::Channel*> active;
    auto result = poller->poll(0, active);

    ASSERT_TRUE(result);
    EXPECT_EQ(result.value(), 0U);
    EXPECT_TRUE(active.empty());
    drain_eventfd(event_fd.get());
}

TEST(EpollPollerTest, EdgeTriggeredChannelRequiresANewReadinessTransition) {
    NullLogger logger;
    auto loop = make_loop(logger);
    auto poller = make_poller();
    auto event_fd = make_event_fd();
    ASSERT_NE(loop, nullptr);
    ASSERT_NE(poller, nullptr);
    ASSERT_TRUE(event_fd.valid());
    iaisf::net::Channel channel{*loop, event_fd.get()};
    channel.enable_reading();
    channel.set_edge_triggered(true);
    ASSERT_TRUE(poller->add(channel));

    signal_eventfd(event_fd.get());
    signal_eventfd(event_fd.get());
    std::vector<iaisf::net::Channel*> active;
    auto first = poller->poll(100, active);
    ASSERT_TRUE(first);
    ASSERT_EQ(first.value(), 1U);
    EXPECT_TRUE(channel.edge_triggered());

    auto second = poller->poll(0, active);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.value(), 0U);

    drain_eventfd(event_fd.get());
    signal_eventfd(event_fd.get());
    auto third = poller->poll(100, active);
    ASSERT_TRUE(third);
    EXPECT_EQ(third.value(), 1U);

    drain_eventfd(event_fd.get());
    EXPECT_TRUE(poller->remove(channel));
}

TEST(EpollPollerTest, RejectsInvalidWaitTimeout) {
    auto poller = make_poller();
    ASSERT_NE(poller, nullptr);
    std::vector<iaisf::net::Channel*> active;

    auto result = poller->poll(-2, active);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::InvalidArgument);
}

TEST(EpollPollerTest, ReusedDescriptorMapsOnlyToTheNewChannel) {
    NullLogger logger;
    auto loop = make_loop(logger);
    auto poller = make_poller();
    auto original_fd = make_event_fd();
    auto replacement_source = make_event_fd();
    ASSERT_NE(loop, nullptr);
    ASSERT_NE(poller, nullptr);
    ASSERT_TRUE(original_fd.valid());
    ASSERT_TRUE(replacement_source.valid());
    const int reused_number = original_fd.get();

    iaisf::net::Channel original_channel{*loop, reused_number};
    original_channel.enable_reading();
    ASSERT_TRUE(poller->add(original_channel));
    ASSERT_TRUE(poller->remove(original_channel));
    original_fd.reset();

    ASSERT_EQ(
        ::dup3(replacement_source.get(), reused_number, O_CLOEXEC),
        reused_number);
    iaisf::net::UniqueFd reused_fd{reused_number};
    iaisf::net::Channel replacement_channel{*loop, reused_fd.get()};
    replacement_channel.enable_reading();
    ASSERT_TRUE(poller->add(replacement_channel));

    signal_eventfd(reused_fd.get());
    std::vector<iaisf::net::Channel*> active;
    auto result = poller->poll(100, active);

    ASSERT_TRUE(result);
    ASSERT_EQ(result.value(), 1U);
    ASSERT_EQ(active.size(), 1U);
    EXPECT_EQ(active.front(), &replacement_channel);
    EXPECT_NE(active.front(), &original_channel);
    drain_eventfd(reused_fd.get());
    EXPECT_TRUE(poller->remove(replacement_channel));
}

}  // namespace
