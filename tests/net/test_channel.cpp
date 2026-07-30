#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "iaisf/logging/logger.hpp"
#include "iaisf/net/channel.hpp"
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

static_assert(!std::is_copy_constructible_v<iaisf::net::Channel>);
static_assert(!std::is_copy_assignable_v<iaisf::net::Channel>);
static_assert(!std::is_move_constructible_v<iaisf::net::Channel>);
static_assert(!std::is_move_assignable_v<iaisf::net::Channel>);

TEST(ChannelTest, DoesNotOwnOrCloseDescriptor) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    const int descriptor = ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(descriptor, 0);
    iaisf::net::UniqueFd owner{descriptor};

    {
        iaisf::net::Channel channel{*loop, descriptor};
        EXPECT_EQ(channel.fd(), descriptor);
    }

    EXPECT_NE(::fcntl(descriptor, F_GETFD), -1);
}

TEST(ChannelTest, ReadSideHangupTriggersReadWithoutImmediateClose) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    iaisf::net::Channel channel{*loop, -1};
    int read_count = 0;
    int close_count = 0;
    channel.set_read_callback([&read_count] { ++read_count; });
    channel.set_close_callback([&close_count] { ++close_count; });

    channel.set_ready_events(
        static_cast<std::uint32_t>(EPOLLRDHUP));
    channel.handle_event();

    EXPECT_EQ(read_count, 1);
    EXPECT_EQ(close_count, 0);
}

TEST(ChannelTest, HangupWithReadableDataRunsReadWithoutImmediateClose) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    iaisf::net::Channel channel{*loop, -1};
    int read_count = 0;
    int close_count = 0;
    channel.set_read_callback([&read_count] { ++read_count; });
    channel.set_close_callback([&close_count] { ++close_count; });

    channel.set_ready_events(
        iaisf::net::Channel::kHangupEvent |
        static_cast<std::uint32_t>(EPOLLIN));
    channel.handle_event();

    EXPECT_EQ(read_count, 1);
    EXPECT_EQ(close_count, 0);
}

TEST(ChannelTest, HangupWithoutReadSideEventRunsCloseOnly) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    iaisf::net::Channel channel{*loop, -1};
    int close_count = 0;
    int read_count = 0;
    channel.set_close_callback([&close_count] { ++close_count; });
    channel.set_read_callback([&read_count] { ++read_count; });

    channel.set_ready_events(
        iaisf::net::Channel::kHangupEvent);
    channel.handle_event();

    EXPECT_EQ(close_count, 1);
    EXPECT_EQ(read_count, 0);
}

TEST(ChannelTest, ErrorPrecedesReadForTheSameReadyEvent) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    iaisf::net::Channel channel{*loop, -1};
    std::vector<std::string> order;
    channel.set_error_callback([&order] { order.emplace_back("error"); });
    channel.set_read_callback([&order] { order.emplace_back("read"); });

    channel.set_ready_events(
        iaisf::net::Channel::kErrorEvent |
        static_cast<std::uint32_t>(EPOLLIN));
    channel.handle_event();

    EXPECT_EQ(order, (std::vector<std::string>{"error", "read"}));
}

TEST(ChannelTest, ReadPrecedesWriteForTheSameReadyEvent) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    iaisf::net::Channel channel{*loop, -1};
    std::vector<std::string> order;
    channel.set_read_callback([&order] { order.emplace_back("read"); });
    channel.set_write_callback([&order] { order.emplace_back("write"); });

    channel.set_ready_events(
        static_cast<std::uint32_t>(EPOLLIN) |
        iaisf::net::Channel::kWriteEvent);
    channel.handle_event();

    EXPECT_EQ(order, (std::vector<std::string>{"read", "write"}));
}

TEST(ChannelTest, EventMaskSupportsReadWriteEdgeAndDisableAll) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    const int descriptor = ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(descriptor, 0);
    iaisf::net::UniqueFd owner{descriptor};
    iaisf::net::Channel channel{*loop, descriptor};

    channel.enable_reading();
    channel.enable_writing();
    channel.set_edge_triggered(true);
    EXPECT_TRUE(channel.has_events());
    EXPECT_TRUE(channel.edge_triggered());
    EXPECT_NE(channel.events() & iaisf::net::Channel::kReadEvents, 0U);
    EXPECT_NE(channel.events() & iaisf::net::Channel::kWriteEvent, 0U);
    ASSERT_TRUE(channel.update());
    EXPECT_TRUE(channel.is_registered());

    channel.disable_writing();
    ASSERT_TRUE(channel.update());
    EXPECT_EQ(channel.events() & iaisf::net::Channel::kWriteEvent, 0U);

    channel.disable_all();
    ASSERT_TRUE(channel.update());
    EXPECT_FALSE(channel.has_events());
    EXPECT_FALSE(channel.is_registered());
}

}  // namespace
