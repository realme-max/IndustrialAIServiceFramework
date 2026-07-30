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
#include <vector>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "iaisf/core/error.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/buffer.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/tcp/tcp_connection.hpp"
#include "iaisf/net/tcp/tcp_server.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace {

using namespace std::chrono_literals;
using iaisf::net::tcp::Buffer;
using iaisf::net::tcp::Ipv4Endpoint;
using iaisf::net::tcp::TcpConnection;
using iaisf::net::tcp::TcpServer;
using iaisf::net::tcp::TcpServerOptions;

class RecordingLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        const std::string_view message) override {
        std::lock_guard<std::mutex> lock{mutex_};
        messages_.emplace_back(message);
    }

    [[nodiscard]] bool contains(const std::string_view fragment) const {
        std::lock_guard<std::mutex> lock{mutex_};
        for (const auto& message : messages_) {
            if (message.find(fragment) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

class ThrowingLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        std::string_view) override {
        ++call_count_;
        throw std::runtime_error{"expected logger failure"};
    }

    [[nodiscard]] std::size_t call_count() const noexcept {
        return call_count_;
    }

private:
    std::size_t call_count_{0U};
};

struct ClientSocket final {
    iaisf::net::UniqueFd fd;
    std::string error;
};

std::unique_ptr<iaisf::net::EventLoop> make_loop(
    iaisf::ILogger& logger,
    const std::size_t pending_capacity = 256U) {
    auto result =
        iaisf::net::EventLoop::create(logger, 128U, pending_capacity);
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

ClientSocket connect_client(
    const Ipv4Endpoint& endpoint,
    const int receive_buffer_size = 0) {
    iaisf::net::UniqueFd descriptor{
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
    if (!descriptor.valid()) {
        return {std::move(descriptor), "socket failed"};
    }

    const timeval timeout{10, 0};
    if (::setsockopt(
            descriptor.get(),
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            static_cast<socklen_t>(sizeof(timeout))) != 0 ||
        ::setsockopt(
            descriptor.get(),
            SOL_SOCKET,
            SO_SNDTIMEO,
            &timeout,
            static_cast<socklen_t>(sizeof(timeout))) != 0) {
        return {std::move(descriptor), "socket timeout option failed"};
    }
    if (receive_buffer_size > 0 &&
        ::setsockopt(
            descriptor.get(),
            SOL_SOCKET,
            SO_RCVBUF,
            &receive_buffer_size,
            static_cast<socklen_t>(sizeof(receive_buffer_size))) != 0) {
        return {std::move(descriptor), "receive buffer option failed"};
    }

    const sockaddr_in address = endpoint.to_sockaddr();
    if (::connect(
            descriptor.get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) != 0) {
        return {std::move(descriptor), "connect failed"};
    }
    return {std::move(descriptor), {}};
}

std::string send_all(
    const int descriptor,
    const std::string_view bytes) {
    std::size_t sent_total = 0U;
    while (sent_total < bytes.size()) {
        const ssize_t sent = ::send(
            descriptor,
            bytes.data() + sent_total,
            bytes.size() - sent_total,
            MSG_NOSIGNAL);
        if (sent > 0) {
            sent_total += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return "send failed";
    }
    return {};
}

std::string receive_exact(
    const int descriptor,
    const std::size_t expected_size,
    std::string& output) {
    output.clear();
    output.resize(expected_size);
    std::size_t received_total = 0U;
    while (received_total < expected_size) {
        const ssize_t received = ::recv(
            descriptor,
            output.data() + received_total,
            expected_size - received_total,
            0);
        if (received > 0) {
            received_total += static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return "receive_exact failed";
    }
    return {};
}

std::string expect_peer_close(const int descriptor) {
    char byte = '\0';
    for (;;) {
        const ssize_t received = ::recv(descriptor, &byte, sizeof(byte), 0);
        if (received == 0) {
            return {};
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && errno == ECONNRESET) {
            return {};
        }
        return "peer did not close as expected";
    }
}

TcpServer::Ptr create_server(
    iaisf::net::EventLoop& loop,
    iaisf::ILogger& logger,
    TcpServerOptions options) {
    auto result = TcpServer::create(
        loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        std::move(options));
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

class ServerCleanupGuard final {
public:
    explicit ServerCleanupGuard(TcpServer::Ptr& server) noexcept
        : server_(server) {}

    ~ServerCleanupGuard() {
        if (!server_) {
            return;
        }
        auto result = server_->stop();
        if (!result) {
            ADD_FAILURE() << result.error().message;
            return;
        }
        server_.reset();
    }

private:
    TcpServer::Ptr& server_;
};

TcpConnection::MessageCallback echo_callback(std::string& error) {
    return [&error](const TcpConnection::Ptr& connection, Buffer& input) {
        const std::size_t length = input.readable_bytes();
        auto send_result = connection->send(input.peek(), length);
        if (!send_result) {
            error = send_result.error().message;
            return;
        }
        auto retrieve_result = input.retrieve(length);
        if (!retrieve_result) {
            error = retrieve_result.error().message;
        }
    };
}

TEST(TcpServerOptionsTest, ProvidesValidatedDefaults) {
    const auto options = TcpServerOptions::defaults();

    EXPECT_GT(options.listen_backlog(), 0);
    EXPECT_GT(options.max_connections(), 0U);
    EXPECT_GT(options.input_initial_capacity(), 0U);
    EXPECT_LE(
        options.input_initial_capacity(),
        options.input_maximum_capacity());
    EXPECT_GT(options.output_initial_capacity(), 0U);
    EXPECT_LE(
        options.output_initial_capacity(),
        options.output_maximum_capacity());
    EXPECT_LE(
        options.output_high_water_mark(),
        options.output_maximum_capacity());
    EXPECT_FALSE(options.socket_send_buffer_bytes().has_value());
}

TEST(TcpServerOptionsTest, RejectsNegativeZeroCrossFieldAndHardLimitValues) {
    auto negative_backlog =
        TcpServerOptions::create(-1, 1, 1, 1, 1, 1, 1);
    auto zero_connections =
        TcpServerOptions::create(1, 0, 1, 1, 1, 1, 1);
    auto input_inverted =
        TcpServerOptions::create(1, 1, 8, 4, 1, 1, 1);
    auto output_inverted =
        TcpServerOptions::create(1, 1, 1, 1, 8, 2, 4);
    auto high_water_too_large =
        TcpServerOptions::create(1, 1, 1, 1, 1, 9, 8);
    auto excessive_connections = TcpServerOptions::create(
        1,
        TcpServerOptions::kMaximumConnections + 1,
        1,
        1,
        1,
        1,
        1);
    auto excessive_buffer = TcpServerOptions::create(
        1,
        1,
        1,
        TcpServerOptions::kMaximumBufferBytes + 1,
        1,
        1,
        1);
    auto zero_socket_send_buffer =
        TcpServerOptions::create(1, 1, 1, 1, 1, 1, 1, 0);
    auto excessive_socket_send_buffer = TcpServerOptions::create(
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        TcpServerOptions::kMaximumBufferBytes + 1);

    EXPECT_FALSE(negative_backlog);
    EXPECT_FALSE(zero_connections);
    EXPECT_FALSE(input_inverted);
    EXPECT_FALSE(output_inverted);
    EXPECT_FALSE(high_water_too_large);
    EXPECT_FALSE(excessive_connections);
    EXPECT_FALSE(excessive_buffer);
    EXPECT_FALSE(zero_socket_send_buffer);
    EXPECT_FALSE(excessive_socket_send_buffer);
}

TEST(TcpServerTest, EchoesBinaryBytesAndClosesAfterPeerHalfClose) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    std::string server_error;
    std::vector<TcpConnection::State> transitions;
    ASSERT_TRUE(server->start(
        echo_callback(server_error),
        [&transitions](const TcpConnection::Ptr& connection) {
            transitions.push_back(connection->state());
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    const std::string payload{"abc\0def\0ghi", 11U};
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, payload, &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(socket.fd.get(), payload);
        }
        if (error.empty() &&
            ::shutdown(socket.fd.get(), SHUT_WR) != 0) {
            error = "client shutdown failed";
        }
        std::string echoed;
        if (error.empty()) {
            error = receive_exact(socket.fd.get(), payload.size(), echoed);
        }
        if (error.empty() && echoed != payload) {
            error = "echoed binary payload differs";
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server_error.empty());
    ASSERT_EQ(transitions.size(), 2U);
    EXPECT_EQ(transitions.front(), TcpConnection::State::Connected);
    EXPECT_EQ(transitions.back(), TcpConnection::State::Disconnected);
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, DeferredRemovalReleasesTheLastConnectionOwner) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};

    std::weak_ptr<TcpConnection> observed_connection;
    bool disconnect_callback_called = false;
    std::promise<bool> released_promise;
    auto released_future = released_promise.get_future().share();
    ASSERT_TRUE(server->start(
        [](const TcpConnection::Ptr&, Buffer& input) {
            input.retrieve_all();
        },
        [
            &loop,
            &observed_connection,
            &disconnect_callback_called,
            &released_promise](const TcpConnection::Ptr& connection) {
            if (connection->state() == TcpConnection::State::Connected) {
                observed_connection = connection;
                return;
            }
            disconnect_callback_called = true;
            auto queued = loop->queue_in_loop([
                &loop,
                observed_connection,
                &released_promise] {
                released_promise.set_value(
                    observed_connection.expired());
                loop->stop();
            });
            if (!queued) {
                released_promise.set_value(false);
                loop->stop();
            }
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        endpoint,
        released_future,
        &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty() &&
            ::shutdown(socket.fd.get(), SHUT_WR) != 0) {
            error = "lifecycle client shutdown failed";
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        if (error.empty() &&
            released_future.wait_for(5s) != std::future_status::ready) {
            error = "deferred connection release timed out";
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
    ASSERT_EQ(released_future.wait_for(0s), std::future_status::ready);
    EXPECT_TRUE(released_future.get());
    EXPECT_TRUE(disconnect_callback_called);
    EXPECT_TRUE(observed_connection.expired());
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, PreservesStreamAcrossFragmentsAndInitialBufferGrowth) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto options_result = TcpServerOptions::create(
        16,
        8,
        32,
        256 * 1024,
        32,
        64 * 1024,
        512 * 1024);
    ASSERT_TRUE(options_result);
    auto server = create_server(
        *loop,
        logger,
        std::move(options_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    std::string server_error;
    ASSERT_TRUE(server->start(echo_callback(server_error)));

    std::string payload(128U * 1024U, '\0');
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(index % 251U);
    }
    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, payload, &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                std::string_view{payload.data(), 1U});
        }
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                std::string_view{payload.data() + 1U, 7U});
        }
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                std::string_view{
                    payload.data() + 8U,
                    payload.size() - 8U});
        }
        if (error.empty() &&
            ::shutdown(socket.fd.get(), SHUT_WR) != 0) {
            error = "fragment client shutdown failed";
        }
        std::string echoed;
        if (error.empty()) {
            error = receive_exact(
                socket.fd.get(),
                payload.size(),
                echoed);
        }
        if (error.empty() && echoed != payload) {
            error = "fragmented stream echo mismatch";
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server_error.empty());
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, DrainsEtAcceptAndReadAcrossMultipleClients) {
    RecordingLogger logger;
    auto loop = make_loop(logger, 512U);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    std::string server_error;
    ASSERT_TRUE(server->start(echo_callback(server_error)));

    constexpr std::size_t kClientCount = 8U;
    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        std::vector<iaisf::net::UniqueFd> sockets;
        sockets.reserve(kClientCount);
        std::string error;
        for (std::size_t index = 0U;
             index < kClientCount && error.empty();
             ++index) {
            ClientSocket socket = connect_client(endpoint);
            error = socket.error;
            sockets.push_back(std::move(socket.fd));
        }
        for (std::size_t index = 0U;
             index < sockets.size() && error.empty();
             ++index) {
            const std::string payload =
                "client-" + std::to_string(index);
            error = send_all(sockets[index].get(), payload);
            if (error.empty() &&
                ::shutdown(sockets[index].get(), SHUT_WR) != 0) {
                error = "client shutdown failed";
            }
            std::string echoed;
            if (error.empty()) {
                error = receive_exact(
                    sockets[index].get(),
                    payload.size(),
                    echoed);
            }
            if (error.empty() && echoed != payload) {
                error = "multi-client echo mismatch";
            }
            if (error.empty()) {
                error = expect_peer_close(sockets[index].get());
            }
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server_error.empty());
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_EQ(server->rejected_connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, RejectsConnectionsBeyondConfiguredMaximum) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto options_result =
        TcpServerOptions::create(16, 1, 64, 1024, 64, 512, 1024);
    ASSERT_TRUE(options_result);
    auto server = create_server(
        *loop,
        logger,
        std::move(options_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    std::string server_error;
    ASSERT_TRUE(server->start(echo_callback(server_error)));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        ClientSocket first = connect_client(endpoint);
        ClientSocket second = connect_client(endpoint);
        std::string error =
            first.error.empty() ? second.error : first.error;
        if (error.empty()) {
            error = expect_peer_close(second.fd.get());
        }
        const std::string payload{"kept"};
        if (error.empty()) {
            error = send_all(first.fd.get(), payload);
        }
        if (error.empty() &&
            ::shutdown(first.fd.get(), SHUT_WR) != 0) {
            error = "first client shutdown failed";
        }
        std::string echoed;
        if (error.empty()) {
            error = receive_exact(
                first.fd.get(),
                payload.size(),
                echoed);
        }
        if (error.empty() && echoed != payload) {
            error = "accepted client echo mismatch";
        }
        if (error.empty()) {
            error = expect_peer_close(first.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server_error.empty());
    EXPECT_EQ(server->rejected_connection_count(), 1U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, FailsClosedWhenInputBufferExceedsHardMaximum) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto options_result =
        TcpServerOptions::create(16, 8, 8, 16, 64, 512, 1024);
    ASSERT_TRUE(options_result);
    auto server = create_server(
        *loop,
        logger,
        std::move(options_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    int message_count = 0;
    ASSERT_TRUE(server->start([
        &message_count](const TcpConnection::Ptr&, Buffer& input) {
        ++message_count;
        input.retrieve_all();
    }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        const std::string oversized(32U, 'x');
        if (error.empty()) {
            error = send_all(socket.fd.get(), oversized);
        }
        if (error.empty() &&
            ::shutdown(socket.fd.get(), SHUT_WR) != 0) {
            error = "client shutdown failed";
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_EQ(message_count, 0);
    EXPECT_TRUE(logger.contains("Buffer maximum capacity exceeded"));
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, EnforcesOutputHardMaximumForSlowClient) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto options_result = TcpServerOptions::create(
        16,
        8,
        64,
        1024,
        1024,
        16 * 1024,
        32 * 1024,
        4096);
    ASSERT_TRUE(options_result);
    auto server = create_server(
        *loop,
        logger,
        std::move(options_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    const std::string response(4U * 1024U * 1024U, 'r');
    std::mutex result_mutex;
    std::condition_variable result_condition;
    bool callback_finished = false;
    bool capacity_rejected = false;
    ASSERT_TRUE(server->start([
        &response,
        &result_mutex,
        &result_condition,
        &callback_finished,
        &capacity_rejected](
            const TcpConnection::Ptr& connection,
            Buffer& input) {
        auto result = connection->send(response);
        {
            std::lock_guard<std::mutex> lock{result_mutex};
            capacity_rejected =
                !result &&
                result.error().code ==
                    iaisf::ErrorCode::ResourceExhausted;
            callback_finished = true;
        }
        input.retrieve_all();
        result_condition.notify_one();
    }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        endpoint,
        &result_mutex,
        &result_condition,
        &callback_finished,
        &client_promise] {
        ClientSocket socket = connect_client(endpoint, 4096);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(socket.fd.get(), "trigger");
        }
        if (error.empty()) {
            std::unique_lock<std::mutex> lock{result_mutex};
            if (!result_condition.wait_for(
                    lock,
                    5s,
                    [&callback_finished] { return callback_finished; })) {
                error = "output limit callback timed out";
            }
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(capacity_rejected);
    EXPECT_TRUE(logger.contains("Buffer maximum capacity exceeded"));
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, RearmsHighWaterAfterOutputDrainsBelowThreshold) {
    RecordingLogger logger;
    auto loop = make_loop(logger, 512U);
    ASSERT_NE(loop, nullptr);
    auto options_result = TcpServerOptions::create(
        16,
        8,
        64,
        1024,
        4096,
        16 * 1024,
        8 * 1024 * 1024,
        4096);
    ASSERT_TRUE(options_result);
    auto server = create_server(
        *loop,
        logger,
        std::move(options_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    const std::string response(4U * 1024U * 1024U, 'h');
    std::string server_error;
    int high_water_count = 0;
    ASSERT_TRUE(server->start(
        [&response, &server_error](
            const TcpConnection::Ptr& connection,
            Buffer& input) {
            auto result = connection->send(response);
            if (!result) {
                server_error = result.error().message;
            }
            input.retrieve_all();
        },
        {},
        [&high_water_count](
            const TcpConnection::Ptr&,
            const std::size_t pending_bytes) {
            EXPECT_GE(pending_bytes, 16U * 1024U);
            ++high_water_count;
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        endpoint,
        response_size = response.size(),
        &client_promise] {
        ClientSocket socket = connect_client(endpoint, 4096);
        std::string error = socket.error;
        std::string received;
        if (error.empty()) {
            error = send_all(socket.fd.get(), "1");
        }
        if (error.empty()) {
            error = receive_exact(
                socket.fd.get(),
                response_size,
                received);
        }
        if (error.empty()) {
            error = send_all(socket.fd.get(), "2");
        }
        if (error.empty() &&
            ::shutdown(socket.fd.get(), SHUT_WR) != 0) {
            error = "client shutdown failed";
        }
        if (error.empty()) {
            error = receive_exact(
                socket.fd.get(),
                response_size,
                received);
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server_error.empty());
    EXPECT_EQ(high_water_count, 2);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, StopClosesExistingConnectionsWithoutStoppingLoopItself) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    std::mutex connected_mutex;
    std::condition_variable connected_condition;
    bool connected = false;
    std::string stop_error;
    ASSERT_TRUE(server->start(
        [](const TcpConnection::Ptr&, Buffer& input) {
            input.retrieve_all();
        },
        [
            &connected_mutex,
            &connected_condition,
            &connected](const TcpConnection::Ptr& connection) {
            if (connection->state() == TcpConnection::State::Connected) {
                {
                    std::lock_guard<std::mutex> lock{connected_mutex};
                    connected = true;
                }
                connected_condition.notify_one();
            }
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        server,
        endpoint,
        &connected_mutex,
        &connected_condition,
        &connected,
        &stop_error,
        &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            std::unique_lock<std::mutex> lock{connected_mutex};
            if (!connected_condition.wait_for(
                    lock,
                    5s,
                    [&connected] { return connected; })) {
                error = "connection callback timed out";
            }
        }
        if (error.empty()) {
            auto queued = loop->queue_in_loop([
                &loop,
                server,
                &stop_error] {
                auto stop_result = server->stop();
                if (!stop_result) {
                    stop_error = stop_result.error().message;
                }
                loop->stop();
            });
            if (!queued) {
                error = queued.error().message;
                loop->stop();
            }
        } else {
            loop->stop();
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        client_promise.set_value(error);
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(stop_error.empty());
    EXPECT_EQ(loop->state(), iaisf::net::EventLoop::State::Stopped);
    EXPECT_FALSE(server->started());
    EXPECT_EQ(server->connection_count(), 0U);
    server.reset();
}

TEST(TcpServerTest, PeerResetDuringOutputDoesNotBreakAHealthyConnection) {
    RecordingLogger logger;
    auto loop = make_loop(logger, 512U);
    ASSERT_NE(loop, nullptr);
    auto options_result = TcpServerOptions::create(
        16,
        8,
        64,
        1024,
        4096,
        64 * 1024,
        8 * 1024 * 1024,
        4096);
    ASSERT_TRUE(options_result);
    auto server = create_server(
        *loop,
        logger,
        std::move(options_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    const std::string large_output(4U * 1024U * 1024U, 'x');
    std::string server_error;
    std::mutex reset_mutex;
    std::condition_variable reset_condition;
    bool reset_output_queued = false;
    ASSERT_TRUE(server->start([
        &large_output,
        &server_error,
        &reset_mutex,
        &reset_condition,
        &reset_output_queued](
            const TcpConnection::Ptr& connection,
            Buffer& input) {
        auto value_result =
            input.retrieve_as_string(input.readable_bytes());
        if (!value_result) {
            server_error = value_result.error().message;
            return;
        }
        if (value_result.value() == "reset") {
            auto reset_send = connection->send(large_output);
            if (!reset_send &&
                reset_send.error().code != iaisf::ErrorCode::SystemError) {
                server_error = reset_send.error().message;
            }
            {
                std::lock_guard<std::mutex> lock{reset_mutex};
                reset_output_queued = true;
            }
            reset_condition.notify_one();
            return;
        }
        auto send_result = connection->send(value_result.value());
        if (!send_result) {
            server_error = send_result.error().message;
        }
    }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        endpoint,
        &reset_mutex,
        &reset_condition,
        &reset_output_queued,
        &client_promise] {
        ClientSocket reset_client = connect_client(endpoint, 4096);
        std::string error = reset_client.error;
        if (error.empty()) {
            error = send_all(reset_client.fd.get(), "reset");
        }
        if (error.empty()) {
            std::unique_lock<std::mutex> lock{reset_mutex};
            if (!reset_condition.wait_for(
                    lock,
                    5s,
                    [&reset_output_queued] {
                        return reset_output_queued;
                    })) {
                error = "reset output callback timed out";
            }
        }
        if (error.empty()) {
            const linger reset_linger{1, 0};
            if (::setsockopt(
                    reset_client.fd.get(),
                    SOL_SOCKET,
                    SO_LINGER,
                    &reset_linger,
                    static_cast<socklen_t>(sizeof(reset_linger))) != 0) {
                error = "setting reset linger failed";
            }
        }
        reset_client.fd.reset();

        ClientSocket healthy = connect_client(endpoint);
        if (error.empty()) {
            error = healthy.error;
        }
        const std::string payload{"healthy"};
        if (error.empty()) {
            error = send_all(healthy.fd.get(), payload);
        }
        if (error.empty() &&
            ::shutdown(healthy.fd.get(), SHUT_WR) != 0) {
            error = "healthy client shutdown failed";
        }
        std::string echoed;
        if (error.empty()) {
            error = receive_exact(
                healthy.fd.get(),
                payload.size(),
                echoed);
        }
        if (error.empty() && echoed != payload) {
            error = "healthy echo mismatch after peer reset";
        }
        if (error.empty()) {
            error = expect_peer_close(healthy.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server_error.empty());
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, MessageCallbackExceptionDoesNotBreakAHealthyConnection) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    ASSERT_TRUE(server->start(
        [](const TcpConnection::Ptr& connection, Buffer& input) {
            auto value_result =
                input.retrieve_as_string(input.readable_bytes());
            if (!value_result) {
                throw std::runtime_error{"failed to consume test input"};
            }
            if (value_result.value() == "throw") {
                throw std::runtime_error{
                    "expected message callback failure"};
            }
            auto send_result = connection->send(value_result.value());
            if (!send_result) {
                throw std::runtime_error{"healthy echo send failed"};
            }
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        ClientSocket failing = connect_client(endpoint);
        std::string error = failing.error;
        if (error.empty()) {
            error = send_all(failing.fd.get(), "throw");
        }
        if (error.empty()) {
            error = expect_peer_close(failing.fd.get());
        }

        ClientSocket healthy = connect_client(endpoint);
        if (error.empty()) {
            error = healthy.error;
        }
        const std::string payload{"healthy"};
        if (error.empty()) {
            error = send_all(healthy.fd.get(), payload);
        }
        if (error.empty() &&
            ::shutdown(healthy.fd.get(), SHUT_WR) != 0) {
            error = "healthy client shutdown failed";
        }
        std::string echoed;
        if (error.empty()) {
            error = receive_exact(
                healthy.fd.get(),
                payload.size(),
                echoed);
        }
        if (error.empty() && echoed != payload) {
            error = "healthy echo mismatch after callback exception";
        }
        if (error.empty()) {
            error = expect_peer_close(healthy.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(logger.contains("expected message callback failure"));
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, CleanupBypassesFullOrdinaryQueueAndRunsOnce) {
    RecordingLogger logger;
    auto loop = make_loop(logger, 1U);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    int disconnect_count = 0;
    bool observer_ran = false;
    bool table_was_empty = false;
    std::string callback_error;
    const std::weak_ptr<TcpServer> weak_server = server;
    ASSERT_TRUE(server->start(
        [
            &loop,
            weak_server,
            &observer_ran,
            &table_was_empty,
            &callback_error](
                const TcpConnection::Ptr& connection,
                Buffer& input) {
            input.retrieve_all();
            auto queued = loop->queue_in_loop([
                &loop,
                weak_server,
                &observer_ran,
                &table_was_empty] {
                observer_ran = true;
                if (const auto locked = weak_server.lock()) {
                    table_was_empty =
                        locked->connection_count() == 0U;
                }
                loop->stop();
            });
            if (!queued) {
                callback_error = queued.error().message;
                loop->stop();
                return;
            }
            if (loop->pending_callback_count() != 1U) {
                callback_error =
                    "ordinary callback queue was not full";
                loop->stop();
                return;
            }
            auto first_close = connection->force_close();
            auto repeated_close = connection->force_close();
            if (!first_close) {
                callback_error = first_close.error().message;
            } else if (!repeated_close) {
                callback_error = repeated_close.error().message;
            }
        },
        [&disconnect_count](const TcpConnection::Ptr& connection) {
            if (connection->state() ==
                TcpConnection::State::Disconnected) {
                ++disconnect_count;
            }
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(socket.fd.get(), "close");
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
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
    EXPECT_TRUE(callback_error.empty());
    EXPECT_TRUE(observer_ran);
    EXPECT_TRUE(table_was_empty);
    EXPECT_EQ(disconnect_count, 1);
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    EXPECT_TRUE(server->stopped());
    server.reset();
}

TEST(TcpServerTest, StopInsideActiveBatchDrainsMultipleConnectionsWithFullQueue) {
    RecordingLogger logger;
    auto loop = make_loop(logger, 1U);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    constexpr std::size_t kConnectionCount = 3U;
    std::size_t connected_count = 0U;
    std::size_t disconnected_count = 0U;
    bool observer_ran = false;
    bool stop_completed = false;
    bool stop_was_deferred = false;
    std::string callback_error;
    const std::weak_ptr<TcpServer> weak_server = server;
    ASSERT_TRUE(server->start(
        [](const TcpConnection::Ptr&, Buffer& input) {
            input.retrieve_all();
        },
        [
            &loop,
            weak_server,
            &connected_count,
            &disconnected_count,
            &observer_ran,
            &stop_completed,
            &stop_was_deferred,
            &callback_error](const TcpConnection::Ptr& connection) {
            if (connection->state() ==
                TcpConnection::State::Disconnected) {
                ++disconnected_count;
                return;
            }
            ++connected_count;
            if (connected_count != kConnectionCount) {
                return;
            }
            auto queued = loop->queue_in_loop([
                &loop,
                weak_server,
                &observer_ran,
                &stop_completed] {
                observer_ran = true;
                if (const auto locked = weak_server.lock()) {
                    stop_completed =
                        locked->stopped() &&
                        locked->connection_count() == 0U;
                }
                loop->stop();
            });
            if (!queued) {
                callback_error = queued.error().message;
                loop->stop();
                return;
            }
            const auto locked = weak_server.lock();
            if (!locked) {
                callback_error = "server lifetime ended unexpectedly";
                loop->stop();
                return;
            }
            auto stop_result = locked->stop();
            if (!stop_result) {
                callback_error = stop_result.error().message;
                loop->stop();
                return;
            }
            stop_was_deferred =
                !locked->stopped() &&
                locked->connection_count() == kConnectionCount;
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        std::vector<iaisf::net::UniqueFd> clients;
        clients.reserve(kConnectionCount);
        std::string error;
        for (std::size_t index = 0U;
             index < kConnectionCount && error.empty();
             ++index) {
            ClientSocket socket = connect_client(endpoint);
            error = socket.error;
            clients.push_back(std::move(socket.fd));
        }
        for (const auto& client_fd : clients) {
            if (error.empty()) {
                error = expect_peer_close(client_fd.get());
            }
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
    EXPECT_TRUE(callback_error.empty());
    EXPECT_TRUE(observer_ran);
    EXPECT_TRUE(stop_completed);
    EXPECT_TRUE(stop_was_deferred);
    EXPECT_EQ(connected_count, kConnectionCount);
    EXPECT_EQ(disconnected_count, kConnectionCount);
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stopped());
    EXPECT_TRUE(server->stop());
    EXPECT_TRUE(server->stop());
    EXPECT_FALSE(connect_client(endpoint).error.empty());
    server.reset();
}

TEST(TcpServerTest, DestructionWhileStartedIsAContractViolation) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    ASSERT_TRUE(server->start(
        [](const TcpConnection::Ptr&, Buffer& input) {
            input.retrieve_all();
        }));

    EXPECT_DEATH_IF_SUPPORTED(server.reset(), ".*");

    ASSERT_NE(server, nullptr);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, EmptyStopIsIdempotentAndPreventsRestart) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};

    EXPECT_TRUE(server->stop());
    EXPECT_TRUE(server->stop());
    EXPECT_TRUE(server->stopped());
    EXPECT_FALSE(server->started());
    EXPECT_EQ(server->connection_count(), 0U);
    auto restart = server->start(
        [](const TcpConnection::Ptr&, Buffer& input) {
            input.retrieve_all();
        });
    ASSERT_FALSE(restart);
    EXPECT_EQ(restart.error().code, iaisf::ErrorCode::InvalidState);
    server.reset();
}

TEST(TcpServerTest, PeerEofDeliversFinalBytesOnceAndDiscardsPartialRemainder) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    int message_count = 0;
    int disconnect_count = 0;
    std::string observed;
    std::string callback_error;
    ASSERT_TRUE(server->start(
        [
            &message_count,
            &observed,
            &callback_error](
                const TcpConnection::Ptr& connection,
                Buffer& input) {
            ++message_count;
            observed.assign(input.peek(), input.readable_bytes());
            auto retrieve_result = input.retrieve(2U);
            if (!retrieve_result) {
                callback_error = retrieve_result.error().message;
                return;
            }
            auto send_result = connection->send("ack");
            if (!send_result) {
                callback_error = send_result.error().message;
            }
        },
        [&disconnect_count](const TcpConnection::Ptr& connection) {
            if (connection->state() ==
                TcpConnection::State::Disconnected) {
                ++disconnect_count;
            }
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(socket.fd.get(), "abcdef");
        }
        if (error.empty() &&
            ::shutdown(socket.fd.get(), SHUT_WR) != 0) {
            error = "partial-consume client shutdown failed";
        }
        std::string response;
        if (error.empty()) {
            error = receive_exact(socket.fd.get(), 3U, response);
        }
        if (error.empty() && response != "ack") {
            error = "partial-consume response mismatch";
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(callback_error.empty());
    EXPECT_EQ(observed, "abcdef");
    EXPECT_EQ(message_count, 1);
    EXPECT_EQ(disconnect_count, 1);
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, PeerEofDoesNotRepeatCallbackWhenInputIsUnconsumed) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    int message_count = 0;
    int disconnect_count = 0;
    std::string observed;
    std::string callback_error;
    ASSERT_TRUE(server->start(
        [
            &message_count,
            &observed,
            &callback_error](
                const TcpConnection::Ptr& connection,
                Buffer& input) {
            ++message_count;
            observed.assign(input.peek(), input.readable_bytes());
            auto send_result = connection->send("ack");
            if (!send_result) {
                callback_error = send_result.error().message;
            }
        },
        [&disconnect_count](const TcpConnection::Ptr& connection) {
            if (connection->state() ==
                TcpConnection::State::Disconnected) {
                ++disconnect_count;
            }
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(socket.fd.get(), "unconsumed");
        }
        if (error.empty() &&
            ::shutdown(socket.fd.get(), SHUT_WR) != 0) {
            error = "unconsumed client shutdown failed";
        }
        std::string response;
        if (error.empty()) {
            error = receive_exact(socket.fd.get(), 3U, response);
        }
        if (error.empty() && response != "ack") {
            error = "unconsumed response mismatch";
        }
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(callback_error.empty());
    EXPECT_EQ(observed, "unconsumed");
    EXPECT_EQ(message_count, 1);
    EXPECT_EQ(disconnect_count, 1);
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(TcpServerTest, ConnectionCallbackExceptionsCannotBlockTableCleanup) {
    ThrowingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto server = create_server(
        *loop,
        logger,
        TcpServerOptions::defaults());
    ASSERT_NE(server, nullptr);
    ServerCleanupGuard server_cleanup{server};
    int connection_callback_count = 0;
    ASSERT_TRUE(server->start(
        [](const TcpConnection::Ptr&, Buffer& input) {
            input.retrieve_all();
        },
        [&connection_callback_count](const TcpConnection::Ptr&) {
            ++connection_callback_count;
            throw std::runtime_error{
                "expected connection callback failure"};
        }));

    const Ipv4Endpoint endpoint = server->local_endpoint();
    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        ClientSocket socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = expect_peer_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run_result = loop->run();
    client.join();

    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_EQ(connection_callback_count, 2);
    EXPECT_GE(logger.call_count(), 2U);
    EXPECT_EQ(server->connection_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

}  // namespace
