#include <cerrno>
#include <chrono>
#include <atomic>
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
#include <dirent.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "iaisf/http/builtin_routes.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_router.hpp"
#include "iaisf/http/http_server.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/tcp/tcp_server.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::http {
namespace {

class RecordingLogger final : public ILogger {
public:
    void log(
        LogLevel,
        std::string_view,
        std::string_view message) override {
        std::lock_guard<std::mutex> lock{mutex_};
        messages_.emplace_back(message);
    }

private:
    std::mutex mutex_;
    std::vector<std::string> messages_;
};

struct ClientSocket {
    net::UniqueFd fd;
    std::string error;
};

struct WireResponse {
    int status{0};
    std::string head;
    std::string body;
};

class ResponseReader final {
public:
    std::string read(int descriptor, WireResponse& response) {
        constexpr std::string_view kSeparator{"\r\n\r\n"};
        std::size_t separator = buffer_.find(kSeparator);
        while (separator == std::string::npos) {
            const auto error = receive_more(descriptor);
            if (!error.empty()) {
                return error;
            }
            separator = buffer_.find(kSeparator);
        }

        const std::size_t head_size = separator + kSeparator.size();
        response.head = buffer_.substr(0U, head_size);
        if (response.head.size() < 12U ||
            response.head.compare(0U, 9U, "HTTP/1.1 ") != 0) {
            return "invalid response status line";
        }
        try {
            response.status = std::stoi(response.head.substr(9U, 3U));
        } catch (...) {
            return "invalid response status code";
        }

        constexpr std::string_view kLength{"Content-Length: "};
        const auto length_start = response.head.find(kLength);
        if (length_start == std::string::npos) {
            return "missing response Content-Length";
        }
        const auto value_start = length_start + kLength.size();
        const auto value_end = response.head.find("\r\n", value_start);
        if (value_end == std::string::npos) {
            return "invalid response Content-Length line";
        }
        std::size_t body_size = 0U;
        try {
            body_size = static_cast<std::size_t>(std::stoull(
                response.head.substr(value_start, value_end - value_start)));
        } catch (...) {
            return "invalid response Content-Length value";
        }

        while (buffer_.size() - head_size < body_size) {
            const auto error = receive_more(descriptor);
            if (!error.empty()) {
                return error;
            }
        }
        response.body = buffer_.substr(head_size, body_size);
        buffer_.erase(0U, head_size + body_size);
        return {};
    }

    [[nodiscard]] bool empty() const noexcept {
        return buffer_.empty();
    }

private:
    std::string receive_more(int descriptor) {
        char bytes[4096];
        for (;;) {
            const ssize_t count =
                ::recv(descriptor, bytes, sizeof(bytes), 0);
            if (count > 0) {
                buffer_.append(bytes, static_cast<std::size_t>(count));
                return {};
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return "response receive failed";
        }
    }

    std::string buffer_;
};

std::unique_ptr<net::EventLoop> make_loop(
    ILogger& logger,
    std::size_t pending_capacity = 256U) {
    auto result =
        net::EventLoop::create(logger, 128U, pending_capacity);
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

Result<HttpRouter> make_builtin_router(HttpLimits limits) {
    HttpRouter router{limits};
    auto builtins = register_builtin_routes(router);
    if (!builtins) {
        return Result<HttpRouter>::failure(std::move(builtins).error());
    }
    auto frozen = router.freeze();
    if (!frozen) {
        return Result<HttpRouter>::failure(std::move(frozen).error());
    }
    return Result<HttpRouter>::success(std::move(router));
}

HttpServer::Ptr make_server(
    net::EventLoop& loop,
    ILogger& logger,
    HttpRouter router,
    HttpLimits limits = HttpLimits::defaults(),
    net::tcp::TcpServerOptions tcp_options =
        net::tcp::TcpServerOptions::defaults()) {
    auto result = HttpServer::create(
        loop,
        logger,
        net::tcp::Ipv4Endpoint::loopback(0U),
        std::move(tcp_options),
        std::move(router),
        std::move(limits));
    EXPECT_TRUE(result);
    return result ? std::move(result).value() : nullptr;
}

ClientSocket connect_client(
    const net::tcp::Ipv4Endpoint& endpoint,
    const int receive_buffer_size = 0) {
    net::UniqueFd descriptor{
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
    if (!descriptor.valid()) {
        return {std::move(descriptor), "client socket failed"};
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
        return {std::move(descriptor), "client timeout option failed"};
    }
    if (receive_buffer_size > 0 &&
        ::setsockopt(
            descriptor.get(),
            SOL_SOCKET,
            SO_RCVBUF,
            &receive_buffer_size,
            static_cast<socklen_t>(sizeof(receive_buffer_size))) != 0) {
        return {std::move(descriptor), "client receive buffer option failed"};
    }
    const auto address = endpoint.to_sockaddr();
    if (::connect(
            descriptor.get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) != 0) {
        return {std::move(descriptor), "client connect failed"};
    }
    return {std::move(descriptor), {}};
}

std::string send_all(int descriptor, std::string_view bytes) {
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

std::string expect_close(int descriptor) {
    char byte = '\0';
    for (;;) {
        const ssize_t received =
            ::recv(descriptor, &byte, sizeof(byte), 0);
        if (received == 0 ||
            (received < 0 && errno == ECONNRESET)) {
            return {};
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return "expected peer close";
    }
}

std::string drain_until_close(int descriptor) {
    char bytes[4096];
    for (;;) {
        const ssize_t received =
            ::recv(descriptor, bytes, sizeof(bytes), 0);
        if (received > 0) {
            continue;
        }
        if (received == 0 ||
            (received < 0 && errno == ECONNRESET)) {
            return {};
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return "connection did not close";
    }
}

std::string request_and_expect(
    const net::tcp::Ipv4Endpoint& endpoint,
    std::string_view request,
    int expected_status,
    std::string_view expected_body = {}) {
    auto socket = connect_client(endpoint);
    std::string error = socket.error;
    if (error.empty()) {
        error = send_all(socket.fd.get(), request);
    }
    ResponseReader reader;
    WireResponse response;
    if (error.empty()) {
        error = reader.read(socket.fd.get(), response);
    }
    if (error.empty() && response.status != expected_status) {
        error = "unexpected HTTP status";
    }
    if (error.empty() && !expected_body.empty() &&
        response.body != expected_body) {
        error = "unexpected HTTP body";
    }
    if (error.empty() &&
        response.head.find("Connection: close\r\n") != std::string::npos) {
        error = expect_close(socket.fd.get());
    }
    return error;
}

std::size_t open_descriptor_count() {
    DIR* const directory = ::opendir("/proc/self/fd");
    if (directory == nullptr) {
        return 0U;
    }
    std::size_t count = 0U;
    while (::readdir(directory) != nullptr) {
        ++count;
    }
    const int close_result = ::closedir(directory);
    if (close_result != 0) {
        return 0U;
    }
    return count;
}

class ServerCleanup final {
public:
    explicit ServerCleanup(HttpServer::Ptr& server) noexcept
        : server_(server) {}

    ~ServerCleanup() {
        if (!server_) {
            return;
        }
        auto stopped = server_->stop();
        if (!stopped) {
            ADD_FAILURE() << stopped.error().message;
        }
        server_.reset();
    }

private:
    HttpServer::Ptr& server_;
};

TEST(HttpServerTest, ServesBuiltinHealthOverLoopback) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto router_result = make_builtin_router(HttpLimits::defaults());
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET /health HTTP/1.1\r\nHost: localhost\r\n"
                "Connection: close\r\n\r\n");
        }
        WireResponse response;
        ResponseReader reader;
        if (error.empty()) {
            error = reader.read(socket.fd.get(), response);
        }
        if (error.empty() &&
            (response.status != 200 ||
             response.body != "{\"status\":\"ok\"}")) {
            error = "unexpected health response";
        }
        if (error.empty()) {
            error = expect_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    EXPECT_TRUE(server->stopped());
    EXPECT_EQ(server->session_count(), 0U);
    server.reset();
}

TEST(HttpServerTest, PipeliningUsesBoundedContinuationAndPreservesOrder) {
    auto limits_result =
        HttpLimits::create(1024, 32, 512, 512, 2048, 32, 1024, 4096, 16, 1);
    ASSERT_TRUE(limits_result);
    auto limits = std::move(limits_result).value();
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto router_result = make_builtin_router(limits);
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value(),
        limits);
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET /health HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /version HTTP/1.1\r\nHost: x\r\n"
                "Connection: close\r\n\r\n");
        }
        ResponseReader reader;
        WireResponse first;
        WireResponse second;
        if (error.empty()) {
            error = reader.read(socket.fd.get(), first);
        }
        if (error.empty()) {
            error = reader.read(socket.fd.get(), second);
        }
        if (error.empty() &&
            (first.body != "{\"status\":\"ok\"}" ||
             second.body.find("\"version\":\"0.1.0\"") ==
                 std::string::npos)) {
            error = "pipelined responses are out of order";
        }
        if (error.empty()) {
            error = expect_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, DataAddedBeforeContinuationRunsIsServedOnceInOrder) {
    auto limits_result =
        HttpLimits::create(1024, 32, 512, 512, 2048, 32, 1024, 4096, 16, 1);
    ASSERT_TRUE(limits_result);
    const auto limits = limits_result.value();
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    std::atomic<int> client_descriptor{-1};
    std::size_t handler_count = 0U;
    bool injection_queued = false;
    std::string injection_error;
    HttpRouter router{limits};
    ASSERT_TRUE(router.add_route(
        "GET",
        "/sequence",
        [&loop,
         &client_descriptor,
         &handler_count,
         &injection_queued,
         &injection_error](const HttpRequest&) {
            ++handler_count;
            if (handler_count == 1U) {
                const int descriptor =
                    client_descriptor.load(std::memory_order_acquire);
                auto queued = loop->queue_in_loop(
                    [descriptor, &injection_error] {
                        injection_error = send_all(
                            descriptor,
                            "GET /sequence HTTP/1.1\r\nHost: x\r\n"
                            "Connection: close\r\n\r\n");
                    });
                injection_queued = static_cast<bool>(queued);
            }
            HttpResponse response;
            response.set_body(std::to_string(handler_count));
            return Result<HttpResponse>::success(std::move(response));
        }));
    ASSERT_TRUE(router.freeze());
    auto server = make_server(
        *loop,
        logger,
        std::move(router),
        limits);
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());

    auto socket = connect_client(server->local_endpoint());
    ASSERT_TRUE(socket.error.empty());
    client_descriptor.store(
        socket.fd.get(),
        std::memory_order_release);
    ASSERT_TRUE(send_all(
        socket.fd.get(),
        "GET /sequence HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /sequence HTTP/1.1\r\nHost: x\r\n\r\n")
                    .empty());

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        socket = std::move(socket),
        &client_promise]() mutable {
        std::string error = socket.error;
        ResponseReader reader;
        for (std::size_t index = 1U;
             index <= 3U && error.empty();
             ++index) {
            WireResponse response;
            error = reader.read(socket.fd.get(), response);
            if (error.empty() &&
                response.body != std::to_string(index)) {
                error = "continuation response order mismatch";
            }
        }
        if (error.empty()) {
            error = expect_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(injection_queued);
    EXPECT_TRUE(injection_error.empty());
    EXPECT_EQ(handler_count, 3U);
    EXPECT_EQ(server->session_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, MalformedPipelineEmitsOneErrorAndCloses) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto router_result = make_builtin_router(HttpLimits::defaults());
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET / HTTP/1.1\nHost: x\n\n"
                "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
        }
        ResponseReader reader;
        WireResponse response;
        if (error.empty()) {
            error = reader.read(socket.fd.get(), response);
        }
        if (error.empty() && response.status != 400) {
            error = "malformed request did not return 400";
        }
        if (error.empty() && !reader.empty()) {
            error = "malformed pipeline emitted more than one response";
        }
        if (error.empty()) {
            error = expect_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, FragmentedPostBodyReachesExactRouteHandler) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    HttpRouter router;
    ASSERT_TRUE(router.add_route(
        "POST",
        "/echo",
        [](const HttpRequest& request) {
            HttpResponse response;
            response.set_body(request.body());
            return Result<HttpResponse>::success(std::move(response));
        }));
    ASSERT_TRUE(router.freeze());
    auto server = make_server(*loop, logger, std::move(router));
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto socket = connect_client(endpoint);
        std::string error = socket.error;
        const std::string body{"he\0lo", 5U};
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "POST /ec");
        }
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "ho HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n");
        }
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "Connection: close\r\n\r\n");
        }
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                std::string_view{body.data(), 2U});
        }
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                std::string_view{body.data() + 2U, body.size() - 2U});
        }
        ResponseReader reader;
        WireResponse response;
        if (error.empty()) {
            error = reader.read(socket.fd.get(), response);
        }
        if (error.empty() &&
            (response.status != 200 || response.body != body)) {
            error = "fragmented POST response mismatch";
        }
        if (error.empty()) {
            error = expect_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, LargeConnectionCloseResponseFlushesBeforeServerEof) {
    auto limits_result = HttpLimits::create(
        1024,
        32,
        512,
        512,
        2048,
        32,
        1024,
        2 * 1024 * 1024,
        16,
        4);
    auto tcp_options = net::tcp::TcpServerOptions::create(
        16,
        8,
        4096,
        1024 * 1024,
        4096,
        64 * 1024,
        2 * 1024 * 1024,
        4096);
    ASSERT_TRUE(limits_result);
    ASSERT_TRUE(tcp_options);
    const auto limits = limits_result.value();
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    HttpRouter router{limits};
    const std::string payload(1024U * 1024U, 'L');
    ASSERT_TRUE(router.add_route(
        "GET",
        "/large",
        [&payload](const HttpRequest&) {
            HttpResponse response;
            response.set_body(payload);
            return Result<HttpResponse>::success(std::move(response));
        }));
    ASSERT_TRUE(router.freeze());
    auto server = make_server(
        *loop,
        logger,
        std::move(router),
        limits,
        std::move(tcp_options).value());
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([
        &loop,
        endpoint,
        expected_size = payload.size(),
        &client_promise] {
        auto socket = connect_client(endpoint, 4096);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET /large HTTP/1.1\r\nHost: x\r\n"
                "Connection: close\r\n\r\n");
        }
        ResponseReader reader;
        WireResponse response;
        if (error.empty()) {
            error = reader.read(socket.fd.get(), response);
        }
        if (error.empty() &&
            (response.status != 200 ||
             response.body.size() != expected_size ||
             response.body.find_first_not_of('L') != std::string::npos)) {
            error = "large close response was truncated";
        }
        if (error.empty()) {
            error = expect_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_EQ(server->session_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, SequentialKeepAliveServesVersion404And405Allow) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto router_result = make_builtin_router(HttpLimits::defaults());
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto socket = connect_client(endpoint);
        std::string error = socket.error;
        ResponseReader reader;
        WireResponse version;
        WireResponse missing;
        WireResponse method;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET /version HTTP/1.1\r\nHost: x\r\n\r\n");
        }
        if (error.empty()) {
            error = reader.read(socket.fd.get(), version);
        }
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET /missing HTTP/1.1\r\nHost: x\r\n\r\n");
        }
        if (error.empty()) {
            error = reader.read(socket.fd.get(), missing);
        }
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "POST /health HTTP/1.1\r\nHost: x\r\n"
                "Connection: close\r\n\r\n");
        }
        if (error.empty()) {
            error = reader.read(socket.fd.get(), method);
        }
        if (error.empty() &&
            (version.status != 200 ||
             version.body.find("\"version\":\"0.1.0\"") ==
                 std::string::npos ||
             missing.status != 404 ||
             method.status != 405 ||
             method.head.find("Allow: GET\r\n") == std::string::npos)) {
            error = "sequential keep-alive response mismatch";
        }
        if (error.empty()) {
            error = expect_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, RejectsFramingAmbiguitiesAndConfiguredLimits) {
    auto limits_result =
        HttpLimits::create(256, 32, 128, 32, 256, 8, 4, 1024, 16, 4);
    ASSERT_TRUE(limits_result);
    auto limits = std::move(limits_result).value();
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto router_result = make_builtin_router(limits);
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value(),
        limits);
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        std::string error = request_and_expect(
            endpoint,
            "GET /health HTTP/1.1\r\n\r\n",
            400);
        if (error.empty()) {
            error = request_and_expect(
                endpoint,
                "POST /health HTTP/1.1\r\nHost: x\r\n"
                "Content-Length: 1\r\nContent-Length: 1\r\n\r\nx",
                400);
        }
        if (error.empty()) {
            error = request_and_expect(
                endpoint,
                "POST /health HTTP/1.1\r\nHost: x\r\n"
                "Transfer-Encoding: chunked\r\n\r\n",
                501);
        }
        if (error.empty()) {
            error = request_and_expect(
                endpoint,
                "POST /health HTTP/1.1\r\nHost: x\r\n"
                "Content-Length: 5\r\n\r\n",
                413);
        }
        if (error.empty()) {
            error = request_and_expect(
                endpoint,
                "GET /health HTTP/1.1\r\nHost: x\r\n"
                "X-Large: 123456789012345678901234567890123\r\n\r\n",
                431);
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    EXPECT_EQ(server->session_count(), 0U);
    server.reset();
}

TEST(HttpServerTest, HandlerErrorAndOversizedResponseReturnClosed500) {
    auto limits_result =
        HttpLimits::create(512, 32, 256, 128, 512, 16, 128, 32, 16, 4);
    ASSERT_TRUE(limits_result);
    auto limits = std::move(limits_result).value();
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    HttpRouter router{limits};
    ASSERT_TRUE(router.add_route(
        "GET",
        "/error",
        [](const HttpRequest&) {
            return Result<HttpResponse>::failure(make_error(
                ErrorCode::InternalError,
                "private failure"));
        }));
    ASSERT_TRUE(router.add_route(
        "GET",
        "/large",
        [](const HttpRequest&) {
            HttpResponse response;
            response.set_body(std::string(33U, 'x'));
            return Result<HttpResponse>::success(std::move(response));
        }));
    ASSERT_TRUE(router.freeze());
    auto server = make_server(
        *loop,
        logger,
        std::move(router),
        limits);
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        std::string error = request_and_expect(
            endpoint,
            "GET /error HTTP/1.1\r\nHost: x\r\n\r\n",
            500,
            "Internal Server Error\n");
        if (error.empty()) {
            error = request_and_expect(
                endpoint,
                "GET /large HTTP/1.1\r\nHost: x\r\n\r\n",
                500,
                "Internal Server Error\n");
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, PeerResetDoesNotBreakFollowingHealthyClient) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto router_result = make_builtin_router(HttpLimits::defaults());
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto reset_client = connect_client(endpoint);
        std::string error = reset_client.error;
        if (error.empty()) {
            error = send_all(
                reset_client.fd.get(),
                "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
        }
        if (error.empty()) {
            const linger reset_linger{1, 0};
            if (::setsockopt(
                    reset_client.fd.get(),
                    SOL_SOCKET,
                    SO_LINGER,
                    &reset_linger,
                    static_cast<socklen_t>(sizeof(reset_linger))) != 0) {
                error = "failed to configure client reset";
            }
        }
        reset_client.fd.reset();
        if (error.empty()) {
            error = request_and_expect(
                endpoint,
                "GET /health HTTP/1.1\r\nHost: x\r\n"
                "Connection: close\r\n\r\n",
                200,
                "{\"status\":\"ok\"}");
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, HandlesMultipleConcurrentLoopbackClients) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto router_result = make_builtin_router(HttpLimits::defaults());
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    constexpr std::size_t kClientCount = 6U;
    std::atomic<std::size_t> remaining{kClientCount};
    std::vector<std::future<std::string>> results;
    std::vector<std::thread> clients;
    results.reserve(kClientCount);
    clients.reserve(kClientCount);
    for (std::size_t index = 0U; index < kClientCount; ++index) {
        std::promise<std::string> promise;
        results.push_back(promise.get_future());
        clients.emplace_back(
            [&loop, endpoint, &remaining, promise = std::move(promise)]() mutable {
                auto error = request_and_expect(
                    endpoint,
                    "GET /health HTTP/1.1\r\nHost: x\r\n"
                    "Connection: close\r\n\r\n",
                    200,
                    "{\"status\":\"ok\"}");
                promise.set_value(error);
                if (remaining.fetch_sub(1U) == 1U) {
                    loop->stop();
                }
            });
    }

    auto run = loop->run();
    for (auto& client : clients) {
        client.join();
    }
    ASSERT_TRUE(run);
    for (auto& result : results) {
        ASSERT_EQ(
            result.wait_for(std::chrono::seconds{0}),
            std::future_status::ready);
        EXPECT_TRUE(result.get().empty());
    }
    EXPECT_TRUE(server->stop());
    EXPECT_EQ(server->session_count(), 0U);
    server.reset();
}

TEST(HttpServerTest, ContinuationQueueExhaustionClosesConnection) {
    auto limits_result =
        HttpLimits::create(512, 32, 256, 128, 512, 16, 128, 1024, 16, 1);
    ASSERT_TRUE(limits_result);
    auto limits = std::move(limits_result).value();
    RecordingLogger logger;
    auto loop = make_loop(logger, 1U);
    ASSERT_NE(loop, nullptr);
    bool handler_filled_queue = false;
    HttpRouter router{limits};
    ASSERT_TRUE(router.add_route(
        "GET",
        "/fill",
        [&loop, &handler_filled_queue](const HttpRequest&) {
            auto queued = loop->queue_in_loop([] {});
            handler_filled_queue = static_cast<bool>(queued);
            HttpResponse response;
            response.set_body("accepted");
            return Result<HttpResponse>::success(std::move(response));
        }));
    ASSERT_TRUE(router.freeze());
    auto server = make_server(
        *loop,
        logger,
        std::move(router),
        limits);
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET /fill HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /fill HTTP/1.1\r\nHost: x\r\n\r\n");
        }
        if (error.empty()) {
            error = drain_until_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(handler_filled_queue);
    EXPECT_EQ(server->session_count(), 0U);
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, ServerStopPreventsPendingPipelineContinuation) {
    auto limits_result =
        HttpLimits::create(512, 32, 256, 128, 512, 16, 128, 1024, 16, 1);
    ASSERT_TRUE(limits_result);
    const auto limits = limits_result.value();
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    std::weak_ptr<HttpServer> weak_server;
    std::size_t handler_count = 0U;
    std::string handler_error;
    HttpRouter router{limits};
    ASSERT_TRUE(router.add_route(
        "GET",
        "/stop",
        [&weak_server, &handler_count, &handler_error](
            const HttpRequest&) {
            ++handler_count;
            const auto server = weak_server.lock();
            if (!server) {
                return Result<HttpResponse>::failure(make_error(
                    ErrorCode::InvalidState,
                    "test server lifetime ended"));
            }
            auto stopped = server->stop();
            if (!stopped) {
                handler_error = stopped.error().message;
            }
            HttpResponse response;
            response.set_body("stopping");
            return Result<HttpResponse>::success(std::move(response));
        }));
    ASSERT_TRUE(router.freeze());
    auto server = make_server(
        *loop,
        logger,
        std::move(router),
        limits);
    ASSERT_NE(server, nullptr);
    weak_server = server;
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET /stop HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /stop HTTP/1.1\r\nHost: x\r\n\r\n");
        }
        if (error.empty()) {
            error = drain_until_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(handler_error.empty());
    EXPECT_EQ(handler_count, 1U);
    EXPECT_TRUE(server->stopped());
    EXPECT_EQ(server->session_count(), 0U);
    server.reset();
}

TEST(HttpServerTest, RepeatedConnectionsReleaseSessionsAndFileDescriptors) {
    const std::size_t descriptors_before = open_descriptor_count();
    ASSERT_GT(descriptors_before, 0U);
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    auto router_result = make_builtin_router(HttpLimits::defaults());
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value());
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        std::string error;
        for (std::size_t index = 0U;
             index < 16U && error.empty();
             ++index) {
            error = request_and_expect(
                endpoint,
                "GET /health HTTP/1.1\r\nHost: x\r\n"
                "Connection: close\r\n\r\n",
                200,
                "{\"status\":\"ok\"}");
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    EXPECT_EQ(server->session_count(), 0U);
    server.reset();
    loop.reset();

    EXPECT_EQ(open_descriptor_count(), descriptors_before);
}

TEST(HttpServerTest, HandlerExceptionReturnsClosed500WithoutDetail) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    HttpRouter router;
    ASSERT_TRUE(router.add_route(
        "GET",
        "/fail",
        [](const HttpRequest&) -> Result<HttpResponse> {
            throw std::runtime_error{"private handler detail"};
        }));
    ASSERT_TRUE(router.freeze());
    auto server = make_server(*loop, logger, std::move(router));
    ASSERT_NE(server, nullptr);
    ServerCleanup cleanup{server};
    ASSERT_TRUE(server->start());
    const auto endpoint = server->local_endpoint();

    std::promise<std::string> client_promise;
    auto client_result = client_promise.get_future();
    std::thread client([&loop, endpoint, &client_promise] {
        auto socket = connect_client(endpoint);
        std::string error = socket.error;
        if (error.empty()) {
            error = send_all(
                socket.fd.get(),
                "GET /fail HTTP/1.1\r\nHost: x\r\n\r\n");
        }
        ResponseReader reader;
        WireResponse response;
        if (error.empty()) {
            error = reader.read(socket.fd.get(), response);
        }
        if (error.empty() &&
            (response.status != 500 ||
             response.body.find("private") != std::string::npos)) {
            error = "handler exception response mismatch";
        }
        if (error.empty()) {
            error = expect_close(socket.fd.get());
        }
        client_promise.set_value(error);
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    ASSERT_TRUE(run);
    EXPECT_TRUE(client_result.get().empty());
    EXPECT_TRUE(server->stop());
    server.reset();
}

TEST(HttpServerTest, RejectsUnfrozenRouterAndStopPreventsRestart) {
    RecordingLogger logger;
    auto loop = make_loop(logger);
    ASSERT_NE(loop, nullptr);
    HttpRouter unfrozen;
    auto rejected = HttpServer::create(
        *loop,
        logger,
        net::tcp::Ipv4Endpoint::loopback(0U),
        net::tcp::TcpServerOptions::defaults(),
        std::move(unfrozen));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::InvalidArgument);

    auto router_result = make_builtin_router(HttpLimits::defaults());
    ASSERT_TRUE(router_result);
    auto server = make_server(
        *loop,
        logger,
        std::move(router_result).value());
    ASSERT_NE(server, nullptr);
    ASSERT_TRUE(server->start());
    EXPECT_TRUE(server->stop());
    EXPECT_TRUE(server->stopped());
    EXPECT_FALSE(server->start());

    auto refused = connect_client(server->local_endpoint());
    EXPECT_FALSE(refused.error.empty());
    server.reset();
}

}  // namespace
}  // namespace iaisf::http
