#include <cerrno>
#include <string>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>

#include "iaisf/core/error.hpp"
#include "iaisf/net/socket.hpp"

namespace {

static_assert(!std::is_copy_constructible_v<iaisf::net::Socket>);
static_assert(!std::is_copy_assignable_v<iaisf::net::Socket>);
static_assert(std::is_nothrow_move_constructible_v<iaisf::net::Socket>);
static_assert(std::is_nothrow_move_assignable_v<iaisf::net::Socket>);

TEST(SocketTest, CreatesNonBlockingCloseOnExecIpv4TcpSocket) {
    auto result = iaisf::net::Socket::create_ipv4_tcp();

    ASSERT_TRUE(result);
    iaisf::net::Socket socket = std::move(result).value();
    ASSERT_TRUE(socket.valid());
    EXPECT_EQ(socket.get(), socket.native_handle());

    const int status_flags = ::fcntl(socket.native_handle(), F_GETFL);
    const int descriptor_flags = ::fcntl(socket.native_handle(), F_GETFD);
    ASSERT_NE(status_flags, -1);
    ASSERT_NE(descriptor_flags, -1);
    EXPECT_NE(status_flags & O_NONBLOCK, 0);
    EXPECT_NE(descriptor_flags & FD_CLOEXEC, 0);
}

TEST(SocketTest, ConfiguresReuseAddress) {
    auto result = iaisf::net::Socket::create_ipv4_tcp();
    ASSERT_TRUE(result);
    iaisf::net::Socket socket = std::move(result).value();

    auto option_result = socket.set_reuse_address();

    ASSERT_TRUE(option_result);
    int option_value = 0;
    socklen_t option_size = static_cast<socklen_t>(sizeof(option_value));
    ASSERT_EQ(
        ::getsockopt(
            socket.native_handle(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &option_value,
            &option_size),
        0);
    EXPECT_EQ(option_value, 1);
}

TEST(SocketTest, MoveReleaseAndResetPreserveSingleOwnership) {
    auto result = iaisf::net::Socket::create_ipv4_tcp();
    ASSERT_TRUE(result);
    iaisf::net::Socket original = std::move(result).value();
    const int descriptor = original.native_handle();

    iaisf::net::Socket moved{std::move(original)};
    EXPECT_FALSE(original.valid());
    EXPECT_EQ(moved.native_handle(), descriptor);

    iaisf::net::UniqueFd released = moved.release();
    EXPECT_FALSE(moved.valid());
    EXPECT_EQ(released.get(), descriptor);

    iaisf::net::Socket reset_target;
    reset_target.reset(std::move(released));
    EXPECT_TRUE(reset_target.valid());
    EXPECT_EQ(reset_target.native_handle(), descriptor);
}

TEST(SocketTest, ShutdownWriteSucceedsForOwnedSocketPairEndpoint) {
    int descriptors[2]{-1, -1};
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            descriptors),
        0);
    iaisf::net::Socket endpoint{iaisf::net::UniqueFd{descriptors[0]}};
    iaisf::net::UniqueFd peer{descriptors[1]};
    ASSERT_TRUE(peer.valid());

    auto result = endpoint.shutdown_write();

    EXPECT_TRUE(result);
}

TEST(SocketTest, InvalidOperationsReturnSystemErrorWithErrnoContext) {
    iaisf::net::Socket invalid;

    auto reuse_result = invalid.set_reuse_address();
    auto shutdown_result = invalid.shutdown_write();

    ASSERT_FALSE(reuse_result);
    ASSERT_FALSE(shutdown_result);
    EXPECT_EQ(reuse_result.error().code, iaisf::ErrorCode::SystemError);
    EXPECT_EQ(shutdown_result.error().code, iaisf::ErrorCode::SystemError);
    EXPECT_NE(reuse_result.error().message.find("errno"), std::string::npos);
    EXPECT_NE(shutdown_result.error().message.find("errno"), std::string::npos);
}

}  // namespace
