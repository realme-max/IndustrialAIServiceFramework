#include <cerrno>
#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "iaisf/http/builtin_routes.hpp"
#include "iaisf/http/http_server.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/metrics/metrics.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/tcp/tcp_server.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace {

using iaisf::http::HttpLimits;
using iaisf::http::HttpRouter;
using iaisf::http::HttpServer;
using iaisf::MetricsRegistry;

class RecordingLogger final : public iaisf::ILogger {
public:
    void log(
        iaisf::LogLevel,
        std::string_view,
        std::string_view) override {}
};

struct WireResponse final {
    int status{0};
    std::string head;
    std::string body;
};

struct ClientResult final {
    std::string error;
    int first_status{0};
    int second_status{0};
};

bool configure_client_timeout(const int descriptor) {
    const timeval timeout{10, 0};
    return ::setsockopt(
               descriptor,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &timeout,
               static_cast<socklen_t>(sizeof(timeout))) == 0 &&
           ::setsockopt(
               descriptor,
               SOL_SOCKET,
               SO_SNDTIMEO,
               &timeout,
               static_cast<socklen_t>(sizeof(timeout))) == 0;
}

std::string send_all(const int descriptor, std::string_view bytes) {
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const ssize_t sent = ::send(
            descriptor,
            bytes.data() + offset,
            bytes.size() - offset,
            MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return "client send failed";
    }
    return {};
}

std::string receive_response(const int descriptor, WireResponse& response) {
    std::string bytes;
    constexpr std::string_view separator{"\r\n\r\n"};
    char chunk[4096];
    std::size_t separator_position = std::string::npos;
    while ((separator_position = bytes.find(separator)) == std::string::npos) {
        const ssize_t received =
            ::recv(descriptor, chunk, sizeof(chunk), 0);
        if (received > 0) {
            bytes.append(chunk, static_cast<std::size_t>(received));
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return "response headers not received";
    }

    const std::size_t head_size = separator_position + separator.size();
    response.head = bytes.substr(0U, head_size);
    if (response.head.size() < 12U ||
        response.head.compare(0U, 9U, "HTTP/1.1 ") != 0) {
        return "invalid response status line";
    }
    try {
        response.status = std::stoi(response.head.substr(9U, 3U));
    } catch (...) {
        return "invalid response status";
    }

    constexpr std::string_view length_prefix{"Content-Length: "};
    const auto length_position = response.head.find(length_prefix);
    if (length_position == std::string::npos) {
        return "missing response content length";
    }
    const auto value_begin = length_position + length_prefix.size();
    const auto value_end = response.head.find("\r\n", value_begin);
    if (value_end == std::string::npos) {
        return "invalid response content length";
    }
    std::size_t body_size = 0U;
    try {
        body_size = static_cast<std::size_t>(std::stoull(
            response.head.substr(value_begin, value_end - value_begin)));
    } catch (...) {
        return "invalid response content length value";
    }

    while (bytes.size() - head_size < body_size) {
        const ssize_t received =
            ::recv(descriptor, chunk, sizeof(chunk), 0);
        if (received > 0) {
            bytes.append(chunk, static_cast<std::size_t>(received));
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return "response body not received";
    }
    response.body = bytes.substr(head_size, body_size);

    if (response.head.find("Connection: close\r\n") != std::string::npos) {
        for (;;) {
            const ssize_t received =
                ::recv(descriptor, chunk, sizeof(chunk), 0);
            if (received == 0 ||
                (received < 0 && errno == ECONNRESET)) {
                break;
            }
            if (received < 0 && errno == EINTR) {
                continue;
            }
            if (received < 0) {
                return "connection did not close";
            }
        }
    }
    return {};
}

iaisf::Result<HttpRouter> make_router() {
    HttpRouter router{HttpLimits::defaults()};
    auto registered = iaisf::http::register_builtin_routes(router);
    if (!registered) {
        return iaisf::Result<HttpRouter>::failure(std::move(registered).error());
    }
    auto frozen = router.freeze();
    if (!frozen) {
        return iaisf::Result<HttpRouter>::failure(std::move(frozen).error());
    }
    return iaisf::Result<HttpRouter>::success(std::move(router));
}

TEST(HttpMetricsTest, CountsRequestsResponsesAndParseErrors) {
    RecordingLogger logger;
    MetricsRegistry metrics;
    auto loop_result = iaisf::net::EventLoop::create(
        logger, 128U, 128U, {}, &metrics);
    ASSERT_TRUE(loop_result);
    auto loop = std::move(loop_result).value();

    auto router_result = make_router();
    ASSERT_TRUE(router_result);
    auto server_result = HttpServer::create(
        *loop,
        logger,
        iaisf::net::tcp::Ipv4Endpoint::loopback(0U),
        iaisf::net::tcp::TcpServerOptions::defaults(),
        std::move(router_result).value(),
        HttpLimits::defaults(),
        std::nullopt,
        std::nullopt,
        &metrics);
    ASSERT_TRUE(server_result);
    auto server = std::move(server_result).value();
    ASSERT_TRUE(server->start());

    const auto endpoint = server->local_endpoint();
    std::promise<ClientResult> client_promise;
    auto client_future = client_promise.get_future();
    std::thread client{[&loop, endpoint, &client_promise] {
        ClientResult result;
        auto perform = [&result, endpoint](
                           std::string_view request,
                           int& status) {
            iaisf::net::UniqueFd descriptor{
                ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
            if (!descriptor.valid() || !configure_client_timeout(descriptor.get())) {
                result.error = "client socket setup failed";
                return;
            }
            const auto address = endpoint.to_sockaddr();
            if (::connect(
                    descriptor.get(),
                    reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) != 0) {
                result.error = "client connect failed";
                return;
            }
            result.error = send_all(descriptor.get(), request);
            if (!result.error.empty()) {
                return;
            }
            WireResponse response;
            result.error = receive_response(descriptor.get(), response);
            status = response.status;
        };

        perform(
            "GET /health HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: close\r\n\r\n",
            result.first_status);
        if (result.error.empty()) {
            perform("GET /health HTTP/1.1\r\n\r\n", result.second_status);
        }
        client_promise.set_value(std::move(result));
        loop->stop();
    }};

    const auto run_result = loop->run();
    client.join();
    const auto client_result = client_future.get();
    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.error.empty()) << client_result.error;
    EXPECT_EQ(client_result.first_status, 200);
    EXPECT_EQ(client_result.second_status, 400);
    ASSERT_TRUE(server->stop());
    server.reset();
    loop.reset();

    auto requests = metrics.get_counter("http_requests_total");
    auto responses = metrics.get_counter("http_responses_total");
    auto parse_errors = metrics.get_counter("http_parse_errors_total");
    auto timeout_408 = metrics.get_counter("http_408_timeout_total");
    ASSERT_TRUE(requests);
    ASSERT_TRUE(responses);
    ASSERT_TRUE(parse_errors);
    ASSERT_TRUE(timeout_408);
    EXPECT_EQ(requests.value()->snapshot(), 1U);
    EXPECT_EQ(responses.value()->snapshot(), 2U);
    EXPECT_EQ(parse_errors.value()->snapshot(), 1U);
    EXPECT_EQ(timeout_408.value()->snapshot(), 0U);
}

TEST(HttpMetricsTest, CountsHeaderTimeoutResponse) {
    RecordingLogger logger;
    MetricsRegistry metrics;
    auto loop_result = iaisf::net::EventLoop::create(
        logger, 128U, 128U, {}, &metrics);
    ASSERT_TRUE(loop_result);
    auto loop = std::move(loop_result).value();

    auto router_result = make_router();
    ASSERT_TRUE(router_result);
    auto server_result = HttpServer::create(
        *loop,
        logger,
        iaisf::net::tcp::Ipv4Endpoint::loopback(0U),
        iaisf::net::tcp::TcpServerOptions::defaults(),
        std::move(router_result).value(),
        HttpLimits::defaults(),
        std::chrono::milliseconds(10),
        std::nullopt,
        &metrics);
    ASSERT_TRUE(server_result);
    auto server = std::move(server_result).value();
    ASSERT_TRUE(server->start());

    const auto endpoint = server->local_endpoint();
    std::promise<ClientResult> client_promise;
    auto client_future = client_promise.get_future();
    std::thread client{[&loop, endpoint, &client_promise] {
        ClientResult result;
        iaisf::net::UniqueFd descriptor{
            ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
        if (!descriptor.valid() || !configure_client_timeout(descriptor.get())) {
            result.error = "client socket setup failed";
        } else {
            const auto address = endpoint.to_sockaddr();
            if (::connect(
                    descriptor.get(),
                    reinterpret_cast<const sockaddr*>(&address),
                    static_cast<socklen_t>(sizeof(address))) != 0) {
                result.error = "client connect failed";
            } else {
                result.error = send_all(
                    descriptor.get(), "GET /health HTTP/1.1\r\nHost: ");
                if (result.error.empty()) {
                    WireResponse response;
                    result.error = receive_response(descriptor.get(), response);
                    result.first_status = response.status;
                }
            }
        }
        client_promise.set_value(std::move(result));
        loop->stop();
    }};

    const auto run_result = loop->run();
    client.join();
    const auto client_result = client_future.get();
    ASSERT_TRUE(run_result);
    EXPECT_TRUE(client_result.error.empty()) << client_result.error;
    EXPECT_EQ(client_result.first_status, 408);
    ASSERT_TRUE(server->stop());
    server.reset();
    loop.reset();

    auto requests = metrics.get_counter("http_requests_total");
    auto responses = metrics.get_counter("http_responses_total");
    auto parse_errors = metrics.get_counter("http_parse_errors_total");
    auto timeout_408 = metrics.get_counter("http_408_timeout_total");
    ASSERT_TRUE(requests);
    ASSERT_TRUE(responses);
    ASSERT_TRUE(parse_errors);
    ASSERT_TRUE(timeout_408);
    EXPECT_EQ(requests.value()->snapshot(), 0U);
    EXPECT_EQ(responses.value()->snapshot(), 1U);
    EXPECT_EQ(parse_errors.value()->snapshot(), 0U);
    EXPECT_EQ(timeout_408.value()->snapshot(), 1U);
}

}  // namespace
