#include <cerrno>
#include <chrono>
#include <cstdint>
#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "iaisf/logging/logger.hpp"
#include "iaisf/metrics/metrics.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/buffer.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/tcp/tcp_server.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace {

using iaisf::MetricsRegistry;
using iaisf::net::EventLoop;
using iaisf::net::tcp::Buffer;
using iaisf::net::tcp::Ipv4Endpoint;
using iaisf::net::tcp::TcpServer;
using iaisf::net::tcp::TcpServerOptions;

class RecordingLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        std::string_view) override {}
};

struct ClientResult final {
    std::string error;
};

std::string read_until_close(const int descriptor) {
    char bytes[256];
    for (;;) {
        const ssize_t count = ::recv(descriptor, bytes, sizeof(bytes), 0);
        if (count == 0 || (count < 0 && errno == ECONNRESET)) {
            return {};
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return "client receive timed out";
        }
        if (count < 0) {
            return "client receive failed";
        }
    }
}

bool configure_client_timeout(const int descriptor) {
    const timeval timeout{10, 0};
    return ::setsockopt(
            descriptor,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            static_cast<socklen_t>(sizeof(timeout))) == 0;
}

TEST(TcpMetricsTest, CountsAcceptedActiveAndClosedConnections) {
    RecordingLogger logger;
    MetricsRegistry metrics;
    auto loop_result = EventLoop::create(logger, 64U, 64U, {}, &metrics);
    ASSERT_TRUE(loop_result);
    auto loop = std::move(loop_result).value();

    auto server_result = TcpServer::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        TcpServerOptions::defaults(),
        &metrics);
    ASSERT_TRUE(server_result);
    auto server = std::move(server_result).value();
    const auto endpoint = server->local_endpoint();
    std::promise<std::int64_t> active_promise;
    auto active_future = active_promise.get_future();
    std::atomic_bool active_reported{false};
    ASSERT_TRUE(server->start(
        [](const TcpServer::ConnectionPtr&, Buffer& input) {
            input.retrieve_all();
        },
        [&loop, &metrics, &active_promise, &active_reported](
            const TcpServer::ConnectionPtr& connection) {
            if (connection->state() ==
                    iaisf::net::tcp::TcpConnection::State::Connected &&
                !active_reported.load(std::memory_order_acquire)) {
                bool expected = false;
                if (!active_reported.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    return;
                }
                const auto active = metrics.get_gauge("tcp_connections_active");
                active_promise.set_value(
                    active ? active.value()->snapshot() : -1);
            }
            if (connection->state() ==
                iaisf::net::tcp::TcpConnection::State::Disconnected) {
                bool expected = false;
                if (active_reported.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    active_promise.set_value(0);
                }
                loop->stop();
            }
        }));

    std::promise<ClientResult> client_promise;
    auto client_future = client_promise.get_future();
    std::thread client{[
        &loop,
        endpoint,
        &client_promise,
        &active_promise,
        &active_reported] {
        iaisf::net::UniqueFd descriptor{
            ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
        ClientResult result;
        if (!descriptor.valid()) {
            result.error = "client socket failed";
        } else {
            if (!configure_client_timeout(descriptor.get())) {
                result.error = "client timeout option failed";
            } else {
                const auto address = endpoint.to_sockaddr();
                if (::connect(
                    descriptor.get(),
                    reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) != 0) {
                    result.error = "client connect failed";
                } else if (::send(descriptor.get(), "x", 1U, MSG_NOSIGNAL) != 1) {
                    result.error = "client send failed";
                } else {
                    ::shutdown(descriptor.get(), SHUT_WR);
                    result.error = read_until_close(descriptor.get());
                }
            }
        }
        const bool failed = !result.error.empty();
        if (failed) {
            bool expected = false;
            if (active_reported.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                active_promise.set_value(0);
            }
        }
        client_promise.set_value(std::move(result));
        if (failed) {
            loop->stop();
        }
    }};

    auto run_result = loop->run();
    client.join();
    const auto client_result = client_future.get();
    ASSERT_TRUE(run_result);
    EXPECT_EQ(active_future.get(), 1);
    EXPECT_TRUE(client_result.error.empty()) << client_result.error;

    auto stop_result = server->stop();
    ASSERT_TRUE(stop_result);
    server.reset();
    loop.reset();

    auto accepted = metrics.get_counter("tcp_connections_accepted_total");
    auto active = metrics.get_gauge("tcp_connections_active");
    auto closed = metrics.get_counter("tcp_connections_closed_total");
    ASSERT_TRUE(accepted);
    ASSERT_TRUE(active);
    ASSERT_TRUE(closed);
    EXPECT_EQ(accepted.value()->snapshot(), 1U);
    EXPECT_EQ(active.value()->snapshot(), 0);
    EXPECT_EQ(closed.value()->snapshot(), 1U);
}

TEST(TcpMetricsTest, CountsIdleTimeoutExactlyOnce) {
    RecordingLogger logger;
    MetricsRegistry metrics;
    auto loop_result = EventLoop::create(logger, 64U, 64U, {}, &metrics);
    ASSERT_TRUE(loop_result);
    auto loop = std::move(loop_result).value();

    auto options_result = TcpServerOptions::create(
        128,
        1024,
        4096,
        1024 * 1024,
        4096,
        64 * 1024,
        1024 * 1024,
        std::nullopt,
        10);
    ASSERT_TRUE(options_result);
    auto server_result = TcpServer::create(
        *loop,
        logger,
        Ipv4Endpoint::loopback(0U),
        std::move(options_result).value(),
        &metrics);
    ASSERT_TRUE(server_result);
    auto server = std::move(server_result).value();
    ASSERT_TRUE(server->start(
        [](const TcpServer::ConnectionPtr&, Buffer&) {},
        [&loop](const TcpServer::ConnectionPtr& connection) {
            if (connection->state() ==
                iaisf::net::tcp::TcpConnection::State::Disconnected) {
                loop->stop();
            }
        }));

    const auto endpoint = server->local_endpoint();
    std::promise<ClientResult> client_promise;
    auto client_future = client_promise.get_future();
    std::thread client{[&loop, endpoint, &client_promise] {
        iaisf::net::UniqueFd descriptor{
            ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
        ClientResult result;
        if (!descriptor.valid()) {
            result.error = "client socket failed";
        } else {
            if (!configure_client_timeout(descriptor.get())) {
                result.error = "client timeout option failed";
            } else {
                const auto address = endpoint.to_sockaddr();
                if (::connect(
                    descriptor.get(),
                    reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) != 0) {
                    result.error = "client connect failed";
                } else {
                    result.error = read_until_close(descriptor.get());
                }
            }
        }
        const bool failed = !result.error.empty();
        client_promise.set_value(std::move(result));
        if (failed) {
            loop->stop();
        }
    }};

    auto run_result = loop->run();
    client.join();
    const auto client_result = client_future.get();
    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.error.empty()) << client_result.error;
    ASSERT_TRUE(server->stop());
    server.reset();
    loop.reset();

    auto idle = metrics.get_counter("tcp_idle_timeout_total");
    auto closed = metrics.get_counter("tcp_connections_closed_total");
    auto active = metrics.get_gauge("tcp_connections_active");
    ASSERT_TRUE(idle);
    ASSERT_TRUE(closed);
    ASSERT_TRUE(active);
    EXPECT_EQ(idle.value()->snapshot(), 1U);
    EXPECT_EQ(closed.value()->snapshot(), 1U);
    EXPECT_EQ(active.value()->snapshot(), 0);
}

}  // namespace
