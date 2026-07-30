#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/acceptor.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace {

using namespace std::chrono_literals;
using iaisf::net::tcp::Acceptor;
using iaisf::net::tcp::Ipv4Endpoint;

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

struct ConnectedClient final {
    iaisf::net::UniqueFd fd;
    std::string error;
};

ConnectedClient connect_client_socket(const Ipv4Endpoint& endpoint) {
    iaisf::net::UniqueFd client{
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
    if (!client.valid()) {
        return {std::move(client), "client socket creation failed"};
    }
    const timeval timeout{2, 0};
    if (::setsockopt(
            client.get(),
            SOL_SOCKET,
            SO_SNDTIMEO,
            &timeout,
            static_cast<socklen_t>(sizeof(timeout))) != 0 ||
        ::setsockopt(
            client.get(),
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            static_cast<socklen_t>(sizeof(timeout))) != 0) {
        return {std::move(client), "setting client timeout failed"};
    }
    const sockaddr_in address = endpoint.to_sockaddr();
    if (::connect(
            client.get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) != 0) {
        return {std::move(client), "client connect failed"};
    }
    return {std::move(client), {}};
}

std::string connect_once(const Ipv4Endpoint& endpoint) {
    return connect_client_socket(endpoint).error;
}

std::string wait_for_peer_close(const int descriptor) {
    char byte = '\0';
    for (;;) {
        const ssize_t received =
            ::recv(descriptor, &byte, sizeof(byte), 0);
        if (received == 0) {
            return {};
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && errno == ECONNRESET) {
            return {};
        }
        return "accepted socket was not closed after callback failure";
    }
}

class AcceptorCleanupGuard final {
public:
    explicit AcceptorCleanupGuard(
        std::unique_ptr<Acceptor>& acceptor) noexcept
        : acceptor_(acceptor) {}

    ~AcceptorCleanupGuard() {
        if (!acceptor_) {
            return;
        }
        auto result = acceptor_->stop();
        if (!result) {
            ADD_FAILURE() << result.error().message;
            return;
        }
        acceptor_.reset();
    }

private:
    std::unique_ptr<Acceptor>& acceptor_;
};

TEST(AcceptorTest, BindsEphemeralLoopbackAndAcceptsNonblockingCloexecSocket) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto acceptor_result = Acceptor::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        16);
    ASSERT_TRUE(acceptor_result);
    auto acceptor = std::move(acceptor_result).value();
    AcceptorCleanupGuard acceptor_cleanup{acceptor};
    EXPECT_NE(acceptor->local_endpoint().port(), 0U);

    int accepted_status_flags = -1;
    int accepted_descriptor_flags = -1;
    std::uint16_t peer_port = 0U;
    std::promise<void> accepted_promise;
    auto accepted_future = accepted_promise.get_future().share();
    ASSERT_TRUE(acceptor->start([
        &loop,
        &accepted_status_flags,
        &accepted_descriptor_flags,
        &peer_port,
        &accepted_promise](
            iaisf::net::Socket socket,
            const Ipv4Endpoint& peer) {
        accepted_status_flags =
            ::fcntl(socket.native_handle(), F_GETFL);
        accepted_descriptor_flags =
            ::fcntl(socket.native_handle(), F_GETFD);
        peer_port = peer.port();
        accepted_promise.set_value();
        loop->stop();
    }));
    auto repeated_start = acceptor->start(
        [](iaisf::net::Socket, const Ipv4Endpoint&) {});
    ASSERT_FALSE(repeated_start);
    EXPECT_EQ(
        repeated_start.error().code,
        iaisf::ErrorCode::InvalidState);

    std::promise<std::string> client_result_promise;
    auto client_result = client_result_promise.get_future();
    const Ipv4Endpoint endpoint = acceptor->local_endpoint();
    std::thread client([
        &loop,
        endpoint,
        accepted_future,
        &client_result_promise] {
        std::string error = connect_once(endpoint);
        if (error.empty() &&
            accepted_future.wait_for(5s) != std::future_status::ready) {
            error = "accept callback timed out";
        }
        client_result_promise.set_value(error);
        if (!error.empty()) {
            loop->stop();
        }
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    ASSERT_EQ(client_result.wait_for(0s), std::future_status::ready);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_NE(accepted_status_flags, -1);
    EXPECT_NE(accepted_descriptor_flags, -1);
    EXPECT_NE(accepted_status_flags & O_NONBLOCK, 0);
    EXPECT_NE(accepted_descriptor_flags & FD_CLOEXEC, 0);
    EXPECT_NE(peer_port, 0U);
    EXPECT_EQ(acceptor->accepted_count(), 1U);
    EXPECT_TRUE(acceptor->stop());
    EXPECT_TRUE(acceptor->stop());
    EXPECT_FALSE(connect_once(endpoint).empty());
}

TEST(AcceptorTest, RejectsInvalidBacklogAndEmptyCallback) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);

    auto zero_backlog = Acceptor::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        0);
    auto excessive_backlog = Acceptor::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        Acceptor::kMaximumBacklog + 1);

    ASSERT_FALSE(zero_backlog);
    ASSERT_FALSE(excessive_backlog);
    auto valid = Acceptor::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        8);
    ASSERT_TRUE(valid);
    auto acceptor = std::move(valid).value();
    AcceptorCleanupGuard acceptor_cleanup{acceptor};
    auto empty_callback = acceptor->start({});
    ASSERT_FALSE(empty_callback);
    EXPECT_EQ(
        empty_callback.error().code,
        iaisf::ErrorCode::InvalidArgument);
    EXPECT_TRUE(acceptor->stop());
}

TEST(AcceptorTest, DrainedListenerReturnsSuccessfulEmptyAcceptResult) {
    auto socket_result = iaisf::net::Socket::create_ipv4_tcp();
    ASSERT_TRUE(socket_result);
    auto socket = std::move(socket_result).value();
    ASSERT_TRUE(socket.set_reuse_address());
    ASSERT_TRUE(socket.bind(Ipv4Endpoint::loopback(0U)));
    ASSERT_TRUE(socket.listen(8));
    auto endpoint_result = socket.local_endpoint();
    ASSERT_TRUE(endpoint_result);
    EXPECT_NE(endpoint_result.value().port(), 0U);

    auto accept_result = iaisf::net::accept_ipv4(socket);

    ASSERT_TRUE(accept_result);
    EXPECT_FALSE(accept_result.value().has_value());
}

TEST(AcceptorTest, DrainsBurstConnectionsWithinOneEdgeTriggeredReadPath) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto acceptor_result = Acceptor::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        16);
    ASSERT_TRUE(acceptor_result);
    auto acceptor = std::move(acceptor_result).value();
    AcceptorCleanupGuard acceptor_cleanup{acceptor};
    constexpr std::size_t kConnectionCount = 6U;
    std::size_t accepted = 0U;
    std::mutex accepted_mutex;
    std::condition_variable accepted_condition;
    ASSERT_TRUE(acceptor->start([
        &loop,
        &accepted,
        &accepted_mutex,
        &accepted_condition](
            iaisf::net::Socket,
            const Ipv4Endpoint&) {
        bool complete = false;
        {
            std::lock_guard<std::mutex> lock{accepted_mutex};
            ++accepted;
            complete = accepted == kConnectionCount;
        }
        accepted_condition.notify_one();
        if (complete) {
            loop->stop();
        }
    }));

    const Ipv4Endpoint endpoint = acceptor->local_endpoint();
    std::promise<std::string> client_result_promise;
    auto client_result = client_result_promise.get_future();
    std::thread client([
        &loop,
        endpoint,
        &accepted,
        &accepted_mutex,
        &accepted_condition,
        &client_result_promise] {
        std::string error;
        for (std::size_t index = 0U;
             index < kConnectionCount && error.empty();
             ++index) {
            error = connect_once(endpoint);
        }
        if (error.empty()) {
            std::unique_lock<std::mutex> lock{accepted_mutex};
            if (!accepted_condition.wait_for(
                    lock,
                    5s,
                    [&accepted] {
                        return accepted == kConnectionCount;
                    })) {
                error = "burst accept callbacks timed out";
            }
        }
        client_result_promise.set_value(error);
        if (!error.empty()) {
            loop->stop();
        }
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_EQ(accepted, kConnectionCount);
    EXPECT_EQ(acceptor->accepted_count(), kConnectionCount);
    EXPECT_TRUE(acceptor->stop());
}

TEST(AcceptorTest, CallbackExceptionClosesSocketAndContinuesAccepting) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto acceptor_result = Acceptor::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        16);
    ASSERT_TRUE(acceptor_result);
    auto acceptor = std::move(acceptor_result).value();
    AcceptorCleanupGuard acceptor_cleanup{acceptor};
    std::size_t callback_count = 0U;
    std::promise<void> second_callback_promise;
    auto second_callback = second_callback_promise.get_future().share();
    ASSERT_TRUE(acceptor->start([
        &loop,
        &callback_count,
        &second_callback_promise](
            iaisf::net::Socket,
            const Ipv4Endpoint&) {
        ++callback_count;
        if (callback_count == 1U) {
            throw std::runtime_error{
                "expected new-connection callback failure"};
        }
        second_callback_promise.set_value();
        loop->stop();
    }));

    const Ipv4Endpoint endpoint = acceptor->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        endpoint,
        second_callback,
        &client_promise] {
        ConnectedClient first = connect_client_socket(endpoint);
        std::string error = first.error;
        if (error.empty()) {
            error = wait_for_peer_close(first.fd.get());
        }
        ConnectedClient second;
        if (error.empty()) {
            second = connect_client_socket(endpoint);
            error = second.error;
        }
        if (error.empty() &&
            second_callback.wait_for(5s) != std::future_status::ready) {
            error = "acceptor did not continue after callback exception";
        }
        client_promise.set_value(error);
        if (!error.empty()) {
            loop->stop();
        }
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_EQ(callback_count, 2U);
    EXPECT_EQ(acceptor->accepted_count(), 2U);
    EXPECT_TRUE(acceptor->stop());
}

TEST(AcceptorTest, StopInsideReadCallbackDefersChannelRemovalSafely) {
    NullLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto acceptor_result = Acceptor::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        16);
    ASSERT_TRUE(acceptor_result);
    auto acceptor = std::move(acceptor_result).value();
    AcceptorCleanupGuard acceptor_cleanup{acceptor};
    std::string stop_error;
    std::promise<void> callback_promise;
    auto callback_finished = callback_promise.get_future().share();
    ASSERT_TRUE(acceptor->start([
        &loop,
        &acceptor,
        &stop_error,
        &callback_promise](
            iaisf::net::Socket,
            const Ipv4Endpoint&) {
        auto stop_result = acceptor->stop();
        if (!stop_result) {
            stop_error = stop_result.error().message;
        }
        callback_promise.set_value();
        loop->stop();
    }));

    const Ipv4Endpoint endpoint = acceptor->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        endpoint,
        callback_finished,
        &client_promise] {
        std::string error = connect_once(endpoint);
        if (error.empty() &&
            callback_finished.wait_for(5s) !=
                std::future_status::ready) {
            error = "acceptor stop callback timed out";
        }
        client_promise.set_value(error);
        if (!error.empty()) {
            loop->stop();
        }
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(stop_error.empty());
    EXPECT_TRUE(acceptor->stopped());
    EXPECT_TRUE(acceptor->stop());
    EXPECT_FALSE(connect_once(endpoint).empty());
}

TEST(AcceptorTest, InvalidSocketOperationsReturnExplicitErrors) {
    iaisf::net::Socket invalid;

    auto no_delay = invalid.set_no_delay();
    auto send_buffer = invalid.set_send_buffer_size(4096);
    auto invalid_send_buffer_size = invalid.set_send_buffer_size(0);
    auto bind = invalid.bind(Ipv4Endpoint::loopback(0U));
    auto listen_backlog = invalid.listen(0);
    auto endpoint = invalid.local_endpoint();
    auto socket_error = invalid.socket_error();
    auto accept = iaisf::net::accept_ipv4(invalid);

    EXPECT_FALSE(no_delay);
    EXPECT_FALSE(send_buffer);
    EXPECT_FALSE(invalid_send_buffer_size);
    EXPECT_FALSE(bind);
    EXPECT_FALSE(listen_backlog);
    EXPECT_FALSE(endpoint);
    EXPECT_FALSE(socket_error);
    EXPECT_FALSE(accept);
    EXPECT_EQ(no_delay.error().code, iaisf::ErrorCode::SystemError);
    EXPECT_EQ(send_buffer.error().code, iaisf::ErrorCode::SystemError);
    EXPECT_EQ(
        invalid_send_buffer_size.error().code,
        iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(bind.error().code, iaisf::ErrorCode::SystemError);
    EXPECT_EQ(
        listen_backlog.error().code,
        iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(endpoint.error().code, iaisf::ErrorCode::SystemError);
    EXPECT_EQ(socket_error.error().code, iaisf::ErrorCode::SystemError);
    EXPECT_EQ(accept.error().code, iaisf::ErrorCode::SystemError);
}

}  // namespace
