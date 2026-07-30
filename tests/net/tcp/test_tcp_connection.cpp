#include <chrono>
#include <cerrno>
#include <cstddef>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "iaisf/core/error.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/socket.hpp"
#include "iaisf/net/tcp/buffer.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/tcp/tcp_connection.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net::tcp {

struct TcpConnectionTestAccess {
    static std::string output_bytes(const TcpConnection& connection) {
        return std::string{
            connection.output_buffer_.peek(),
            connection.output_buffer_.readable_bytes()};
    }

    static int native_handle(const TcpConnection& connection) noexcept {
        return connection.socket_.native_handle();
    }
};

}  // namespace iaisf::net::tcp

namespace {

using namespace std::chrono_literals;
using iaisf::net::tcp::Buffer;
using iaisf::net::tcp::Ipv4Endpoint;
using iaisf::net::tcp::TcpConnection;

class NullLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        std::string_view) override {}
};

std::unique_ptr<iaisf::net::EventLoop> make_loop(NullLogger& logger) {
    auto result = iaisf::net::EventLoop::create(logger, 32U, 64U);
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

struct ConnectionPair final {
    TcpConnection::Ptr connection;
    iaisf::net::UniqueFd peer;
};

class ConnectionCleanupGuard final {
public:
    explicit ConnectionCleanupGuard(
        TcpConnection::Ptr& connection) noexcept
        : connection_(connection) {}

    ~ConnectionCleanupGuard() {
        if (!connection_) {
            return;
        }
        if (connection_->state() !=
            TcpConnection::State::Disconnected) {
            auto result = connection_->connect_destroyed();
            if (!result) {
                ADD_FAILURE() << result.error().message;
                return;
            }
        }
        connection_.reset();
    }

private:
    TcpConnection::Ptr& connection_;
};

ConnectionPair make_connection(
    iaisf::net::EventLoop& loop,
    NullLogger& logger,
    const std::size_t output_initial_capacity = 64U,
    const std::size_t output_maximum_capacity = 1024U,
    const std::size_t output_high_water_mark = 512U) {
    int descriptors[2]{-1, -1};
    EXPECT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            descriptors),
        0);
    iaisf::net::Socket socket{
        iaisf::net::UniqueFd{descriptors[0]}};
    iaisf::net::UniqueFd peer{descriptors[1]};
    auto connection_result = TcpConnection::create(
        loop,
        logger,
        1U,
        std::move(socket),
        Ipv4Endpoint::loopback(1000U),
        Ipv4Endpoint::loopback(2000U),
        64U,
        1024U,
        output_initial_capacity,
        output_maximum_capacity,
        output_high_water_mark);
    EXPECT_TRUE(connection_result);
    return {
        connection_result ? std::move(connection_result).value() : nullptr,
        std::move(peer)};
}

bool configure_callbacks(
    iaisf::net::EventLoop& loop,
    const TcpConnection::Ptr& connection,
    int& close_count,
    int& transition_count) {
    auto message_result = connection->set_message_callback(
        [](const TcpConnection::Ptr&, Buffer& input) {
            input.retrieve_all();
        });
    if (!message_result) {
        ADD_FAILURE() << message_result.error().message;
        return false;
    }
    auto connection_result = connection->set_connection_callback(
        [&transition_count](const TcpConnection::Ptr&) {
            ++transition_count;
        });
    if (!connection_result) {
        ADD_FAILURE() << connection_result.error().message;
        return false;
    }
    auto close_result = connection->set_close_callback([
        &loop,
        &close_count](const TcpConnection::Ptr& closing) {
        ++close_count;
        auto queued = loop.queue_in_loop([&loop, closing] {
            auto destroyed = closing->connect_destroyed();
            EXPECT_TRUE(destroyed);
            loop.stop();
        });
        EXPECT_TRUE(queued);
        if (!queued) {
            loop.stop();
        }
    });
    if (!close_result) {
        ADD_FAILURE() << close_result.error().message;
        return false;
    }
    return true;
}

std::string receive_exact_bytes(
    const int descriptor,
    const std::string_view expected) {
    std::string bytes(expected.size(), '\0');
    std::size_t received_total = 0U;
    while (received_total < expected.size()) {
        const ssize_t received = ::recv(
            descriptor,
            bytes.data() + received_total,
            expected.size() - received_total,
            0);
        if (received > 0) {
            received_total += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return "direct connection receive failed";
    }
    return bytes == expected
               ? std::string{}
               : std::string{"direct connection payload mismatch"};
}

TEST(TcpConnectionTest, RejectsInvalidSocketIdentifierAndBufferLimits) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);

    auto invalid_socket = TcpConnection::create(
        *loop,
        logger,
        1U,
        iaisf::net::Socket{},
        Ipv4Endpoint::loopback(1U),
        Ipv4Endpoint::loopback(2U),
        1U,
        1U,
        1U,
        1U,
        1U);
    auto socket_result = iaisf::net::Socket::create_ipv4_tcp();
    ASSERT_TRUE(socket_result);
    auto zero_identifier = TcpConnection::create(
        *loop,
        logger,
        0U,
        std::move(socket_result).value(),
        Ipv4Endpoint::loopback(1U),
        Ipv4Endpoint::loopback(2U),
        1U,
        1U,
        1U,
        1U,
        1U);
    int descriptors[2]{-1, -1};
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            descriptors),
        0);
    iaisf::net::UniqueFd peer{descriptors[1]};
    auto invalid_limits = TcpConnection::create(
        *loop,
        logger,
        2U,
        iaisf::net::Socket{iaisf::net::UniqueFd{descriptors[0]}},
        Ipv4Endpoint::loopback(1U),
        Ipv4Endpoint::loopback(2U),
        8U,
        4U,
        1U,
        1U,
        1U);

    ASSERT_FALSE(invalid_socket);
    ASSERT_FALSE(zero_identifier);
    ASSERT_FALSE(invalid_limits);
    EXPECT_EQ(
        invalid_socket.error().code,
        iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(
        zero_identifier.error().code,
        iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(
        invalid_limits.error().code,
        iaisf::ErrorCode::InvalidArgument);
}

TEST(TcpConnectionTest, RepeatedForceCloseNotifiesAndDestroysExactlyOnce) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto pair = make_connection(*loop, logger);
    ASSERT_NE(pair.connection, nullptr);
    ConnectionCleanupGuard connection_cleanup{pair.connection};
    int close_count = 0;
    int transition_count = 0;
    ASSERT_TRUE(configure_callbacks(
        *loop,
        pair.connection,
        close_count,
        transition_count));
    ASSERT_TRUE(pair.connection->connect_established());

    auto first_close = pair.connection->force_close();
    if (!first_close) {
        ADD_FAILURE() << first_close.error().message;
        EXPECT_TRUE(pair.connection->connect_destroyed());
        return;
    }
    EXPECT_TRUE(pair.connection->force_close());
    auto run_result = loop->run();
    if (pair.connection->state() != TcpConnection::State::Disconnected) {
        EXPECT_TRUE(pair.connection->connect_destroyed());
    }

    ASSERT_TRUE(run_result);
    EXPECT_EQ(close_count, 1);
    EXPECT_EQ(transition_count, 2);
    EXPECT_EQ(
        pair.connection->state(),
        TcpConnection::State::Disconnected);
}

TEST(TcpConnectionTest, NonOwnerThreadCannotSend) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto pair = make_connection(*loop, logger);
    ASSERT_NE(pair.connection, nullptr);
    ConnectionCleanupGuard connection_cleanup{pair.connection};
    int close_count = 0;
    int transition_count = 0;
    ASSERT_TRUE(configure_callbacks(
        *loop,
        pair.connection,
        close_count,
        transition_count));
    ASSERT_TRUE(pair.connection->connect_established());

    std::promise<iaisf::ErrorCode> error_promise;
    auto error = error_promise.get_future();
    std::thread non_owner([
        connection = pair.connection,
        &error_promise] {
        auto result = connection->send("not allowed");
        error_promise.set_value(
            result ? iaisf::ErrorCode::InternalError
                   : result.error().code);
    });
    non_owner.join();
    ASSERT_EQ(error.wait_for(0s), std::future_status::ready);
    EXPECT_EQ(error.get(), iaisf::ErrorCode::InvalidState);

    auto close_result = pair.connection->force_close();
    if (!close_result) {
        ADD_FAILURE() << close_result.error().message;
        EXPECT_TRUE(pair.connection->connect_destroyed());
        return;
    }
    auto run_result = loop->run();
    if (pair.connection->state() != TcpConnection::State::Disconnected) {
        EXPECT_TRUE(pair.connection->connect_destroyed());
    }
    ASSERT_TRUE(run_result);
    EXPECT_EQ(close_count, 1);
    EXPECT_EQ(transition_count, 2);
}

TEST(TcpConnectionTest, OversizedSendAcceptsNoPrefixAndPreservesQueuedOutput) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto pair = make_connection(*loop, logger, 4U, 8U, 4U);
    ASSERT_NE(pair.connection, nullptr);
    ConnectionCleanupGuard connection_cleanup{pair.connection};
    int close_count = 0;
    int transition_count = 0;
    ASSERT_TRUE(configure_callbacks(
        *loop,
        pair.connection,
        close_count,
        transition_count));
    ASSERT_TRUE(pair.connection->connect_established());
    ASSERT_TRUE(pair.connection->send("queued"));
    ASSERT_EQ(pair.connection->output_readable_bytes(), 6U);
    const std::string output_before =
        iaisf::net::tcp::TcpConnectionTestAccess::output_bytes(
            *pair.connection);

    auto rejected = pair.connection->send("xyz");

    ASSERT_FALSE(rejected);
    EXPECT_EQ(
        rejected.error().code,
        iaisf::ErrorCode::ResourceExhausted);
    EXPECT_EQ(pair.connection->output_readable_bytes(), 6U);
    EXPECT_EQ(
        iaisf::net::tcp::TcpConnectionTestAccess::output_bytes(
            *pair.connection),
        output_before);
    EXPECT_EQ(
        pair.connection->state(),
        TcpConnection::State::Disconnecting);
    char byte = '\0';
    errno = 0;
    EXPECT_EQ(::recv(pair.peer.get(), &byte, sizeof(byte), 0), -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    auto while_disconnecting = pair.connection->send("later");
    ASSERT_FALSE(while_disconnecting);
    EXPECT_EQ(
        while_disconnecting.error().code,
        iaisf::ErrorCode::InvalidState);

    auto run_result = loop->run();

    ASSERT_TRUE(run_result);
    EXPECT_EQ(close_count, 1);
    EXPECT_EQ(transition_count, 2);
    EXPECT_EQ(
        pair.connection->state(),
        TcpConnection::State::Disconnected);
    auto while_disconnected = pair.connection->send("later");
    ASSERT_FALSE(while_disconnected);
    EXPECT_EQ(
        while_disconnected.error().code,
        iaisf::ErrorCode::InvalidState);
}

TEST(TcpConnectionTest, GracefulShutdownFlushesWriteHalfThenWaitsForPeerEof) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto pair = make_connection(*loop, logger);
    ASSERT_NE(pair.connection, nullptr);
    ConnectionCleanupGuard connection_cleanup{pair.connection};
    int close_count = 0;
    int transition_count = 0;
    ASSERT_TRUE(configure_callbacks(
        *loop,
        pair.connection,
        close_count,
        transition_count));
    ASSERT_TRUE(pair.connection->connect_established());

    auto shutdown_result = pair.connection->shutdown();
    if (!shutdown_result) {
        ADD_FAILURE() << shutdown_result.error().message;
        EXPECT_TRUE(pair.connection->connect_destroyed());
        return;
    }
    char byte = '\0';
    EXPECT_EQ(::recv(pair.peer.get(), &byte, sizeof(byte), 0), 0);
    ASSERT_EQ(::shutdown(pair.peer.get(), SHUT_WR), 0);

    auto run_result = loop->run();
    if (pair.connection->state() != TcpConnection::State::Disconnected) {
        EXPECT_TRUE(pair.connection->connect_destroyed());
    }

    ASSERT_TRUE(run_result);
    EXPECT_EQ(close_count, 1);
    EXPECT_EQ(transition_count, 2);
    EXPECT_TRUE(pair.connection->peer_eof_received());
    EXPECT_EQ(
        pair.connection->state(),
        TcpConnection::State::Disconnected);
}

TEST(TcpConnectionTest, CloseAfterWriteFlushesFullyRejectsSendAndClosesOnce) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto pair = make_connection(
        *loop,
        logger,
        4096U,
        512U * 1024U,
        16U * 1024U);
    ASSERT_NE(pair.connection, nullptr);
    ConnectionCleanupGuard connection_cleanup{pair.connection};
    const int small_send_buffer = 4096;
    ASSERT_EQ(
        ::setsockopt(
            iaisf::net::tcp::TcpConnectionTestAccess::native_handle(
                *pair.connection),
            SOL_SOCKET,
            SO_SNDBUF,
            &small_send_buffer,
            static_cast<socklen_t>(sizeof(small_send_buffer))),
        0);
    const int peer_flags = ::fcntl(pair.peer.get(), F_GETFL);
    ASSERT_NE(peer_flags, -1);
    ASSERT_EQ(
        ::fcntl(pair.peer.get(), F_SETFL, peer_flags & ~O_NONBLOCK),
        0);
    const timeval timeout{10, 0};
    ASSERT_EQ(
        ::setsockopt(
            pair.peer.get(),
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            static_cast<socklen_t>(sizeof(timeout))),
        0);

    int close_count = 0;
    int transition_count = 0;
    ASSERT_TRUE(configure_callbacks(
        *loop,
        pair.connection,
        close_count,
        transition_count));
    ASSERT_TRUE(pair.connection->connect_established());
    const std::string payload(256U * 1024U, 'c');
    ASSERT_TRUE(pair.connection->send(payload));
    ASSERT_GT(pair.connection->output_readable_bytes(), 0U);

    ASSERT_TRUE(pair.connection->close_after_write());
    EXPECT_TRUE(pair.connection->close_after_write());
    auto rejected = pair.connection->send("late");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, iaisf::ErrorCode::InvalidState);

    std::promise<std::string> peer_promise;
    auto peer_result = peer_promise.get_future();
    std::thread peer_thread([
        &loop,
        peer_descriptor = pair.peer.get(),
        &payload,
        &peer_promise] {
        std::string error =
            receive_exact_bytes(peer_descriptor, payload);
        if (error.empty()) {
            char byte = '\0';
            const ssize_t received =
                ::recv(peer_descriptor, &byte, sizeof(byte), 0);
            if (received != 0) {
                error = "close-after-write did not produce EOF";
            }
        }
        peer_promise.set_value(error);
        if (!error.empty()) {
            loop->stop();
        }
    });

    auto run_result = loop->run();
    peer_thread.join();
    if (pair.connection->state() != TcpConnection::State::Disconnected) {
        EXPECT_TRUE(pair.connection->connect_destroyed());
    }

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(peer_result.get().empty());
    EXPECT_EQ(pair.connection->output_readable_bytes(), 0U);
    EXPECT_EQ(close_count, 1);
    EXPECT_EQ(transition_count, 2);
    EXPECT_EQ(
        pair.connection->state(),
        TcpConnection::State::Disconnected);
}

TEST(TcpConnectionTest, DynamicWriteInterestDrainsAndRearmsHighWater) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    int descriptors[2]{-1, -1};
    ASSERT_EQ(
        ::socketpair(
            AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
            0,
            descriptors),
        0);
    const int small_send_buffer = 4096;
    ASSERT_EQ(
        ::setsockopt(
            descriptors[0],
            SOL_SOCKET,
            SO_SNDBUF,
            &small_send_buffer,
            static_cast<socklen_t>(sizeof(small_send_buffer))),
        0);
    const int peer_flags = ::fcntl(descriptors[1], F_GETFL);
    ASSERT_NE(peer_flags, -1);
    ASSERT_EQ(
        ::fcntl(
            descriptors[1],
            F_SETFL,
            peer_flags & ~O_NONBLOCK),
        0);
    const timeval timeout{10, 0};
    ASSERT_EQ(
        ::setsockopt(
            descriptors[1],
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            static_cast<socklen_t>(sizeof(timeout))),
        0);

    iaisf::net::Socket socket{
        iaisf::net::UniqueFd{descriptors[0]}};
    iaisf::net::UniqueFd peer{descriptors[1]};
    auto connection_result = TcpConnection::create(
        *loop,
        logger,
        7U,
        std::move(socket),
        Ipv4Endpoint::loopback(1000U),
        Ipv4Endpoint::loopback(2000U),
        64U,
        1024U,
        4096U,
        512U * 1024U,
        16U * 1024U);
    ASSERT_TRUE(connection_result);
    auto connection = std::move(connection_result).value();
    ConnectionCleanupGuard connection_cleanup{connection};
    int close_count = 0;
    int transition_count = 0;
    int high_water_count = 0;
    std::string owner_send_error;
    ASSERT_TRUE(configure_callbacks(
        *loop,
        connection,
        close_count,
        transition_count));
    ASSERT_TRUE(connection->set_high_water_callback([
        &high_water_count](
            const TcpConnection::Ptr&,
            const std::size_t pending_bytes) {
        EXPECT_GE(pending_bytes, 16U * 1024U);
        ++high_water_count;
        throw std::runtime_error{
            "expected high-water callback failure"};
    }));
    ASSERT_TRUE(connection->connect_established());
    const std::string payload(256U * 1024U, 'p');

    auto first_send = connection->send(payload);
    if (!first_send) {
        ADD_FAILURE() << first_send.error().message;
        EXPECT_TRUE(connection->connect_destroyed());
        return;
    }
    EXPECT_TRUE(connection->writing_enabled());
    EXPECT_GE(connection->output_readable_bytes(), 16U * 1024U);
    EXPECT_EQ(high_water_count, 1);

    std::promise<std::string> peer_promise;
    auto peer_result = peer_promise.get_future();
    std::thread peer_thread([
        &loop,
        connection,
        peer_descriptor = peer.get(),
        &payload,
        &owner_send_error,
        &peer_promise] {
        std::string error =
            receive_exact_bytes(peer_descriptor, payload);
        if (error.empty()) {
            auto queued = loop->queue_in_loop([
                connection,
                &payload,
                &owner_send_error] {
                auto result = connection->send(payload);
                if (!result) {
                    owner_send_error = result.error().message;
                }
            });
            if (!queued) {
                error = queued.error().message;
            }
        }
        if (error.empty()) {
            error = receive_exact_bytes(peer_descriptor, payload);
        }
        if (error.empty() &&
            ::shutdown(peer_descriptor, SHUT_WR) != 0) {
            error = "direct peer shutdown failed";
        }
        if (error.empty()) {
            char byte = '\0';
            const ssize_t received =
                ::recv(peer_descriptor, &byte, sizeof(byte), 0);
            if (received != 0) {
                error = "direct peer did not observe connection close";
            }
        }
        peer_promise.set_value(error);
        if (!error.empty()) {
            loop->stop();
        }
    });

    auto run_result = loop->run();
    peer_thread.join();
    if (connection->state() != TcpConnection::State::Disconnected) {
        ASSERT_TRUE(connection->connect_destroyed());
    }

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(peer_result.get().empty());
    EXPECT_TRUE(owner_send_error.empty());
    EXPECT_EQ(high_water_count, 2);
    EXPECT_EQ(close_count, 1);
    EXPECT_EQ(transition_count, 2);
    EXPECT_EQ(
        connection->state(),
        TcpConnection::State::Disconnected);
}

}  // namespace
