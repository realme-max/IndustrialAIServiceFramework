#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/unique_fd.hpp"
#include "iaisf/plugin/echo_plugin.hpp"
#include "iaisf/service/industrial_ai_service.hpp"
#include "iaisf/service/service_options.hpp"

namespace iaisf::service {
namespace {

class RecordingLogger final : public ILogger {
public:
    void log(LogLevel, std::string_view, std::string_view message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.emplace_back(message);
    }

private:
    std::mutex mutex_;
    std::vector<std::string> messages_;
};

struct BlockingPluginState {
    BlockingPluginState()
        : started_future(started.get_future().share()),
          release_future(release.get_future().share()) {}

    std::promise<void> started;
    std::shared_future<void> started_future;
    std::promise<void> release;
    std::shared_future<void> release_future;
    std::atomic<bool> signalled{false};
    std::atomic<bool> released{false};

    void release_once() {
        if (!released.exchange(true, std::memory_order_acq_rel)) {
            release.set_value();
        }
    }
};

class BlockingPlugin final : public plugin::IAlgorithmPlugin {
public:
    explicit BlockingPlugin(std::shared_ptr<BlockingPluginState> state)
        : state_(std::move(state)) {}

    plugin::PluginMetadata metadata() const override {
        return plugin::PluginMetadata{
            "test.block",
            "Blocking Test Plugin",
            "1.0.0",
            "Test-only deterministic worker gate.",
            true,
            {"test"}};
    }

    Result<void> validate_input(const nlohmann::json& input) const override {
        if (!input.is_object() || !input.empty()) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "blocking test input must be an empty object"));
        }
        return Result<void>::success();
    }

    Result<nlohmann::json> execute(
        const nlohmann::json& input) const override {
        auto valid = validate_input(input);
        if (!valid) {
            return Result<nlohmann::json>::failure(
                std::move(valid).error());
        }
        if (!state_->signalled.exchange(true, std::memory_order_acq_rel)) {
            state_->started.set_value();
        }
        state_->release_future.wait();
        return Result<nlohmann::json>::success({{"released", true}});
    }

private:
    std::shared_ptr<BlockingPluginState> state_;
};

struct StopTriggerState {
    StopTriggerState() : invoked_future(invoked.get_future().share()) {}

    std::function<Result<void>()> stop;
    std::function<void()> after_stop;
    std::promise<void> invoked;
    std::shared_future<void> invoked_future;
    std::atomic<bool> signalled{false};
};

class StopTriggerPlugin final : public plugin::IAlgorithmPlugin {
public:
    explicit StopTriggerPlugin(std::shared_ptr<StopTriggerState> state)
        : state_(std::move(state)) {}

    plugin::PluginMetadata metadata() const override {
        return plugin::PluginMetadata{
            "test.stop",
            "Stop Trigger Test Plugin",
            "1.0.0",
            "Test-only owner-thread stop trigger.",
            true,
            {"test"}};
    }

    Result<void> validate_input(const nlohmann::json& input) const override {
        if (!input.is_object() || !input.empty()) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidArgument,
                "stop trigger input must be an empty object"));
        }
        if (!state_->signalled.exchange(true, std::memory_order_acq_rel)) {
            auto stopped = state_->stop();
            if (state_->after_stop) {
                state_->after_stop();
            }
            state_->invoked.set_value();
            if (!stopped) {
                return stopped;
            }
        }
        return Result<void>::success();
    }

    Result<nlohmann::json> execute(
        const nlohmann::json&) const override {
        return Result<nlohmann::json>::success({{"stopping", true}});
    }

private:
    std::shared_ptr<StopTriggerState> state_;
};

struct WireResponse {
    int status{0};
    std::string body;
};

class ResponseReader final {
public:
    std::string read(const int descriptor, WireResponse& response) {
        constexpr std::string_view kSeparator{"\r\n\r\n"};
        auto separator = bytes_.find(kSeparator);
        while (separator == std::string::npos) {
            auto error = receive_more(descriptor);
            if (!error.empty()) {
                return error;
            }
            separator = bytes_.find(kSeparator);
        }
        const std::size_t body_start = separator + kSeparator.size();
        if (bytes_.size() < 12U ||
            bytes_.compare(0U, 9U, "HTTP/1.1 ") != 0) {
            return "invalid status line";
        }
        try {
            response.status = std::stoi(bytes_.substr(9U, 3U));
        } catch (...) {
            return "invalid status";
        }
        constexpr std::string_view kLength{"Content-Length: "};
        const auto length_start = bytes_.find(kLength);
        if (length_start == std::string::npos) {
            return "missing content length";
        }
        const auto value_start = length_start + kLength.size();
        const auto value_end = bytes_.find("\r\n", value_start);
        if (value_end == std::string::npos) {
            return "invalid content length";
        }
        std::size_t body_size = 0U;
        try {
            body_size = static_cast<std::size_t>(std::stoull(
                bytes_.substr(value_start, value_end - value_start)));
        } catch (...) {
            return "invalid content length value";
        }
        while (bytes_.size() - body_start < body_size) {
            auto error = receive_more(descriptor);
            if (!error.empty()) {
                return error;
            }
        }
        response.body = bytes_.substr(body_start, body_size);
        bytes_.erase(0U, body_start + body_size);
        return {};
    }

private:
    std::string receive_more(const int descriptor) {
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
            if (count > 0) {
                bytes_.append(buffer, static_cast<std::size_t>(count));
                return {};
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return "response receive failed";
        }
    }

    std::string bytes_;
};

Result<net::UniqueFd> connect_client(
    const net::tcp::Ipv4Endpoint& endpoint) {
    net::UniqueFd descriptor{
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
    if (!descriptor.valid()) {
        return Result<net::UniqueFd>::failure(make_error(
            ErrorCode::IoError,
            "client socket failed"));
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
        return Result<net::UniqueFd>::failure(make_error(
            ErrorCode::IoError,
            "client timeout setup failed"));
    }
    const auto address = endpoint.to_sockaddr();
    if (::connect(
            descriptor.get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) != 0) {
        return Result<net::UniqueFd>::failure(make_error(
            ErrorCode::IoError,
            "client connect failed"));
    }
    return Result<net::UniqueFd>::success(std::move(descriptor));
}

struct OccupiedListener {
    net::UniqueFd descriptor;
    net::tcp::Ipv4Endpoint endpoint;
};

Result<OccupiedListener> occupy_loopback_port() {
    net::UniqueFd descriptor{
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
    if (!descriptor.valid()) {
        return Result<OccupiedListener>::failure(make_error(
            ErrorCode::IoError,
            "occupied listener socket failed"));
    }
    auto address = net::tcp::Ipv4Endpoint::loopback(0U).to_sockaddr();
    if (::bind(
            descriptor.get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) != 0 ||
        ::listen(descriptor.get(), 8) != 0) {
        return Result<OccupiedListener>::failure(make_error(
            ErrorCode::IoError,
            "occupied listener bind failed"));
    }
    socklen_t address_size = static_cast<socklen_t>(sizeof(address));
    if (::getsockname(
            descriptor.get(),
            reinterpret_cast<sockaddr*>(&address),
            &address_size) != 0) {
        return Result<OccupiedListener>::failure(make_error(
            ErrorCode::IoError,
            "occupied listener endpoint failed"));
    }
    auto endpoint = net::tcp::Ipv4Endpoint::from_sockaddr(address);
    if (!endpoint) {
        return Result<OccupiedListener>::failure(
            std::move(endpoint).error());
    }
    return Result<OccupiedListener>::success(OccupiedListener{
        std::move(descriptor),
        std::move(endpoint).value()});
}

std::string send_all(const int descriptor, const std::string_view bytes) {
    std::size_t sent = 0U;
    while (sent < bytes.size()) {
        const ssize_t count = ::send(
            descriptor,
            bytes.data() + sent,
            bytes.size() - sent,
            MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        return "client send failed";
    }
    return {};
}

std::string wait_for_eof(const int descriptor) {
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::recv(descriptor, buffer, sizeof(buffer), 0);
        if (count > 0) {
            continue;
        }
        if (count == 0) {
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        return "client close wait failed";
    }
}

std::string post_request(
    const std::string_view body,
    const std::string_view content_type = "application/json",
    const bool close = true) {
    std::string request{"POST /v1/tasks HTTP/1.1\r\nHost: localhost\r\n"};
    if (!content_type.empty()) {
        request.append("Content-Type: ");
        request.append(content_type);
        request.append("\r\n");
    }
    request.append("Content-Length: ");
    request.append(std::to_string(body.size()));
    request.append(close ? "\r\nConnection: close\r\n\r\n"
                         : "\r\n\r\n");
    request.append(body);
    return request;
}

struct ExchangeResult {
    Result<void> loop_result{Result<void>::success()};
    std::string client_error;
    WireResponse response;
};

ExchangeResult exchange_once(const std::string& wire_request) {
    RecordingLogger logger;
    auto loop_result = net::EventLoop::create(logger, 128U, 256U);
    EXPECT_TRUE(loop_result);
    auto loop = std::move(loop_result).value();
    auto options = ServiceOptions::defaults();
    EXPECT_TRUE(options);
    auto service_result = IndustrialAiService::create(
        *loop,
        logger,
        net::tcp::Ipv4Endpoint::loopback(0U),
        std::move(options).value());
    EXPECT_TRUE(service_result);
    auto service = std::move(service_result).value();
    EXPECT_TRUE(service->start());
    const auto endpoint = service->local_endpoint();

    std::promise<std::pair<std::string, WireResponse>> promise;
    auto future = promise.get_future();
    std::thread client([&loop, endpoint, wire_request, &promise] {
        std::string error;
        WireResponse response;
        auto socket = connect_client(endpoint);
        if (!socket) {
            error = socket.error().message;
        } else {
            error = send_all(socket.value().get(), wire_request);
            ResponseReader reader;
            if (error.empty()) {
                error = reader.read(socket.value().get(), response);
            }
        }
        promise.set_value({std::move(error), std::move(response)});
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    auto client_result = future.get();
    EXPECT_TRUE(service->stop());
    EXPECT_TRUE(service->stopped());
    service.reset();
    return {
        std::move(run),
        std::move(client_result.first),
        std::move(client_result.second)};
}

TEST(ServiceOptionsTest, DefaultsAreCrossLimitConsistent) {
    EXPECT_TRUE(ServiceOptions::defaults());
}

TEST(ServiceOptionsTest, RejectsPoolAndHttpFramingBeforeServiceCreation) {
    const auto defaults = ServiceOptions::defaults().value();
    EXPECT_FALSE(ServiceOptions::create(
        defaults.tcp_options(),
        defaults.http_limits(),
        task::ThreadPoolOptions{
            task::BoundedThreadPool::kMaximumWorkerCount + 1U,
            1U},
        defaults.task_limits(),
        defaults.plugin_limits(),
        defaults.api_limits()));

    const auto narrow_headers = http::HttpLimits::create(
        16 * 1024,
        32,
        8 * 1024,
        32,
        32 * 1024,
        100,
        1024 * 1024,
        1024 * 1024,
        256,
        16);
    ASSERT_TRUE(narrow_headers);
    auto invalid = ServiceOptions::create(
        defaults.tcp_options(),
        narrow_headers.value(),
        defaults.pool_options(),
        defaults.task_limits(),
        defaults.plugin_limits(),
        defaults.api_limits());
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::InvalidArgument);
}

TEST(IndustrialAiServiceTest, CreatedServiceCanStopWithoutStarting) {
    RecordingLogger logger;
    auto loop = net::EventLoop::create(logger).value();
    auto service = IndustrialAiService::create(
                       *loop,
                       logger,
                       net::tcp::Ipv4Endpoint::loopback(0U),
                       ServiceOptions::defaults().value())
                       .value();
    EXPECT_EQ(service->state(), IndustrialAiService::State::Created);
    EXPECT_TRUE(service->stop());
    EXPECT_TRUE(service->stopped());
    EXPECT_EQ(loop->state(), net::EventLoop::State::Created);
}

TEST(IndustrialAiServiceTest, StartAndStopAreSingleUseAndIdempotent) {
    RecordingLogger logger;
    auto loop = net::EventLoop::create(logger).value();
    auto service = IndustrialAiService::create(
                       *loop,
                       logger,
                       net::tcp::Ipv4Endpoint::loopback(0U),
                       ServiceOptions::defaults().value())
                       .value();
    ASSERT_TRUE(service->start());
    EXPECT_FALSE(service->start());
    EXPECT_TRUE(service->stop());
    EXPECT_TRUE(service->stop());
    EXPECT_FALSE(service->start());
}

TEST(
    IndustrialAiServiceTest,
    ActiveHttpStopWaitsForDeferredCleanupBeforeJoiningTasks) {
    RecordingLogger logger;
    auto loop = net::EventLoop::create(logger, 128U, 256U).value();
    auto defaults = ServiceOptions::defaults().value();
    auto options = ServiceOptions::create(
                       defaults.tcp_options(),
                       defaults.http_limits(),
                       task::ThreadPoolOptions{1U, 2U},
                       defaults.task_limits(),
                       defaults.plugin_limits(),
                       defaults.api_limits(),
                       false,
                       false)
                       .value();
    auto blocking = std::make_shared<BlockingPluginState>();
    auto trigger = std::make_shared<StopTriggerState>();
    auto service = IndustrialAiService::create(
                       *loop,
                       logger,
                       net::tcp::Ipv4Endpoint::loopback(0U),
                       std::move(options),
                       {
                           std::make_shared<BlockingPlugin>(blocking),
                           std::make_shared<StopTriggerPlugin>(trigger),
                       })
                       .value();
    trigger->stop = [&service] { return service->stop(); };
    std::atomic<bool> http_cleanup_completed_at_stop{false};
    std::atomic<bool> task_shutdown_started_at_stop{false};
    trigger->after_stop = [&service,
                           &http_cleanup_completed_at_stop,
                           &task_shutdown_started_at_stop] {
        http_cleanup_completed_at_stop.store(
            service->session_count() == 0U &&
                service->connection_count() == 0U,
            std::memory_order_release);
        task_shutdown_started_at_stop.store(
            !service->task_manager().accepting(),
            std::memory_order_release);
    };
    ASSERT_TRUE(service->start());
    const auto endpoint = service->local_endpoint();

    std::promise<std::string> client_result;
    auto client_future = client_result.get_future();
    std::thread client([
        &loop,
        endpoint,
        blocking,
        trigger,
        &client_result] {
        std::string error;
        auto socket = connect_client(endpoint);
        ResponseReader reader;
        if (!socket) {
            error = socket.error().message;
        }
        if (error.empty()) {
            error = send_all(
                socket.value().get(),
                post_request(
                    R"({"operation":"test.block","input":{}})",
                    "application/json",
                    false));
        }
        WireResponse accepted;
        if (error.empty()) {
            error = reader.read(socket.value().get(), accepted);
        }
        if (error.empty() && accepted.status != 202) {
            error = "blocking task was not accepted";
        }
        if (error.empty() &&
            blocking->started_future.wait_for(std::chrono::seconds{5}) !=
                std::future_status::ready) {
            error = "blocking task did not start";
        }
        if (error.empty()) {
            error = send_all(
                socket.value().get(),
                post_request(
                    R"({"operation":"test.stop","input":{}})",
                    "application/json",
                    false));
        }
        if (error.empty() &&
            trigger->invoked_future.wait_for(std::chrono::seconds{5}) !=
                std::future_status::ready) {
            error = "owner-thread stop trigger did not run";
        }
        // TcpServer::stop force-closes active connections. The stop-triggering
        // request therefore has no response contract; verify only that the
        // peer observes the ordered close after the blocked task is released.
        blocking->release_once();
        if (error.empty()) {
            error = wait_for_eof(socket.value().get());
        }
        client_result.set_value(std::move(error));
        loop->stop();
    });

    const auto run = loop->run();
    client.join();
    EXPECT_TRUE(run);
    const auto client_error = client_future.get();
    EXPECT_TRUE(client_error.empty()) << client_error;
    EXPECT_FALSE(http_cleanup_completed_at_stop.load(std::memory_order_acquire));
    EXPECT_FALSE(task_shutdown_started_at_stop.load(std::memory_order_acquire));
    EXPECT_TRUE(service->stopped());
    EXPECT_EQ(service->state(), IndustrialAiService::State::Stopped);
    EXPECT_EQ(service->session_count(), 0U);
    EXPECT_EQ(service->connection_count(), 0U);
    EXPECT_TRUE(service->task_manager().stopped());
    EXPECT_TRUE(service->stop());
    EXPECT_FALSE(service->start());
    trigger->stop = {};
    service.reset();
}

TEST(IndustrialAiServiceTest, OccupiedPortRollsBackAllOwnedComponents) {
    RecordingLogger logger;
    auto loop = net::EventLoop::create(logger).value();
    auto occupied = occupy_loopback_port();
    ASSERT_TRUE(occupied);

    auto state = std::make_shared<BlockingPluginState>();
    auto plugin = std::make_shared<BlockingPlugin>(state);
    std::weak_ptr<const plugin::IAlgorithmPlugin> weak_plugin = plugin;
    std::vector<std::shared_ptr<const plugin::IAlgorithmPlugin>> plugins;
    plugins.push_back(std::move(plugin));
    auto failed = IndustrialAiService::create(
        *loop,
        logger,
        occupied.value().endpoint,
        ServiceOptions::defaults().value(),
        std::move(plugins));
    EXPECT_FALSE(failed);
    EXPECT_TRUE(weak_plugin.expired());

    occupied.value().descriptor.reset();
    auto replacement = IndustrialAiService::create(
                           *loop,
                           logger,
                           net::tcp::Ipv4Endpoint::loopback(0U),
                           ServiceOptions::defaults().value())
                           .value();
    EXPECT_TRUE(replacement->start());
    EXPECT_TRUE(replacement->stop());
    EXPECT_TRUE(replacement->stopped());
}

TEST(IndustrialAiServiceTest, DuplicateStaticPluginRollsBackBeforeWorkersStart) {
    RecordingLogger logger;
    auto loop = net::EventLoop::create(logger).value();
    auto duplicate = std::make_shared<plugin::EchoPlugin>();
    std::weak_ptr<const plugin::IAlgorithmPlugin> weak_duplicate = duplicate;
    std::vector<std::shared_ptr<const plugin::IAlgorithmPlugin>> plugins;
    plugins.push_back(std::move(duplicate));

    auto failed = IndustrialAiService::create(
        *loop,
        logger,
        net::tcp::Ipv4Endpoint::loopback(0U),
        ServiceOptions::defaults().value(),
        std::move(plugins));
    EXPECT_FALSE(failed);
    EXPECT_TRUE(weak_duplicate.expired());

    auto replacement = IndustrialAiService::create(
                           *loop,
                           logger,
                           net::tcp::Ipv4Endpoint::loopback(0U),
                           ServiceOptions::defaults().value())
                           .value();
    EXPECT_TRUE(replacement->start());
    EXPECT_TRUE(replacement->stop());
}

TEST(IndustrialAiServiceTest, StartFailureIsTerminalAndFullyRolledBack) {
    RecordingLogger logger;
    auto loop = net::EventLoop::create(logger).value();
    auto service = IndustrialAiService::create(
                       *loop,
                       logger,
                       net::tcp::Ipv4Endpoint::loopback(0U),
                       ServiceOptions::defaults().value())
                       .value();
    loop->stop();
    EXPECT_FALSE(service->start());
    EXPECT_TRUE(service->stopped());
    EXPECT_TRUE(service->task_manager().stopped());
    EXPECT_EQ(service->session_count(), 0U);
    EXPECT_EQ(service->connection_count(), 0U);
    EXPECT_FALSE(service->start());
    service.reset();

    auto fresh_loop = net::EventLoop::create(logger).value();
    auto replacement = IndustrialAiService::create(
                           *fresh_loop,
                           logger,
                           net::tcp::Ipv4Endpoint::loopback(0U),
                           ServiceOptions::defaults().value())
                           .value();
    EXPECT_TRUE(replacement->start());
    EXPECT_TRUE(replacement->stop());
}

TEST(IndustrialAiServiceTest, ServesHealthOverLoopback) {
    const auto result = exchange_once(
        "GET /health HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n\r\n");
    EXPECT_TRUE(result.loop_result);
    EXPECT_TRUE(result.client_error.empty()) << result.client_error;
    EXPECT_EQ(result.response.status, 200);
    EXPECT_EQ(result.response.body, "{\"status\":\"ok\"}");
}

TEST(IndustrialAiServiceTest, ServesVersionOverLoopback) {
    const auto result = exchange_once(
        "GET /version HTTP/1.1\r\nHost: localhost\r\n"
        "Connection: close\r\n\r\n");
    EXPECT_TRUE(result.client_error.empty()) << result.client_error;
    EXPECT_EQ(result.response.status, 200);
    EXPECT_NE(result.response.body.find("\"version\":\"0.1.0\""), std::string::npos);
}

TEST(IndustrialAiServiceTest, AcceptsEchoTaskOverLoopback) {
    const auto result = exchange_once(post_request(
        R"({"operation":"echo","input":{"payload":{"value":1}}})"));
    EXPECT_TRUE(result.client_error.empty()) << result.client_error;
    EXPECT_EQ(result.response.status, 202);
    const auto body = nlohmann::json::parse(result.response.body);
    EXPECT_TRUE(body["task_id"].is_string());
    EXPECT_FALSE(body.contains("input"));
}

TEST(IndustrialAiServiceTest, AcceptsMockVisionTaskOverLoopback) {
    const auto result = exchange_once(post_request(
        R"({"operation":"mock_vision.detect","input":{"image_id":"sample","width":100,"height":20}})"));
    EXPECT_TRUE(result.client_error.empty()) << result.client_error;
    EXPECT_EQ(result.response.status, 202);
}

TEST(IndustrialAiServiceTest, EchoTaskCanBePolledToSucceededResult) {
    RecordingLogger logger;
    auto loop = net::EventLoop::create(logger, 128U, 256U).value();
    auto service = IndustrialAiService::create(
                       *loop,
                       logger,
                       net::tcp::Ipv4Endpoint::loopback(0U),
                       ServiceOptions::defaults().value())
                       .value();
    ASSERT_TRUE(service->start());
    const auto endpoint = service->local_endpoint();

    std::promise<std::pair<std::string, nlohmann::json>> promise;
    auto future = promise.get_future();
    std::thread client([&loop, endpoint, &promise] {
        std::string error;
        nlohmann::json terminal;
        auto socket = connect_client(endpoint);
        ResponseReader reader;
        WireResponse accepted;
        if (!socket) {
            error = socket.error().message;
        }
        if (error.empty()) {
            error = send_all(
                socket.value().get(),
                post_request(
                    R"({"operation":"echo","input":{"payload":{"answer":42}}})",
                    "application/json",
                    false));
        }
        if (error.empty()) {
            error = reader.read(socket.value().get(), accepted);
        }
        std::string status_url;
        if (error.empty() && accepted.status == 202) {
            try {
                status_url =
                    nlohmann::json::parse(accepted.body)
                        .at("status_url")
                        .get<std::string>();
            } catch (...) {
                error = "invalid accepted response";
            }
        } else if (error.empty()) {
            error = "task was not accepted";
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (error.empty() &&
               std::chrono::steady_clock::now() < deadline) {
            std::string request{"GET "};
            request.append(status_url);
            request.append(
                " HTTP/1.1\r\nHost: localhost\r\n\r\n");
            error = send_all(socket.value().get(), request);
            WireResponse response;
            if (error.empty()) {
                error = reader.read(socket.value().get(), response);
            }
            if (error.empty() && response.status != 200) {
                error = "task query failed";
            }
            if (error.empty()) {
                try {
                    terminal = nlohmann::json::parse(response.body);
                } catch (...) {
                    error = "invalid task query response";
                }
            }
            if (error.empty() && terminal.value("state", "") == "succeeded") {
                break;
            }
            std::this_thread::yield();
        }
        if (error.empty() && terminal.value("state", "") != "succeeded") {
            error = "task did not reach a terminal state before deadline";
        }
        promise.set_value({std::move(error), std::move(terminal)});
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    const auto result = future.get();
    EXPECT_TRUE(run);
    EXPECT_TRUE(result.first.empty()) << result.first;
    EXPECT_EQ(result.second["result"]["answer"], 42);
    EXPECT_FALSE(result.second.contains("input"));
    EXPECT_TRUE(service->stop());
    service.reset();
}

TEST(IndustrialAiServiceTest, QueueFullMapsTo503WithoutBlockingEventLoop) {
    RecordingLogger logger;
    auto loop = net::EventLoop::create(logger, 128U, 256U).value();
    auto defaults = ServiceOptions::defaults().value();
    auto options = ServiceOptions::create(
                       defaults.tcp_options(),
                       defaults.http_limits(),
                       task::ThreadPoolOptions{1U, 1U},
                       defaults.task_limits(),
                       defaults.plugin_limits(),
                       defaults.api_limits(),
                       false,
                       false)
                       .value();
    auto state = std::make_shared<BlockingPluginState>();
    auto service = IndustrialAiService::create(
                       *loop,
                       logger,
                       net::tcp::Ipv4Endpoint::loopback(0U),
                       std::move(options),
                       {std::make_shared<BlockingPlugin>(state)})
                       .value();
    ASSERT_TRUE(service->start());
    const auto endpoint = service->local_endpoint();

    std::promise<std::pair<std::string, WireResponse>> promise;
    auto future = promise.get_future();
    std::thread client([&loop, endpoint, state, &promise] {
        std::string error;
        WireResponse third;
        auto socket = connect_client(endpoint);
        if (!socket) {
            error = socket.error().message;
        }
        ResponseReader reader;
        const std::string request = post_request(
            R"({"operation":"test.block","input":{}})",
            "application/json",
            false);
        for (int index = 0; index < 3 && error.empty(); ++index) {
            error = send_all(socket.value().get(), request);
            WireResponse response;
            if (error.empty()) {
                error = reader.read(socket.value().get(), response);
            }
            if (index == 0 && error.empty()) {
                if (response.status != 202 ||
                    state->started_future.wait_for(
                        std::chrono::seconds{5}) !=
                        std::future_status::ready) {
                    error = "blocking task did not start";
                }
            } else if (index == 1 && error.empty() &&
                       response.status != 202) {
                error = "queued task was not accepted";
            } else if (index == 2) {
                third = std::move(response);
            }
        }
        state->release_once();
        promise.set_value({std::move(error), std::move(third)});
        loop->stop();
    });

    auto run = loop->run();
    client.join();
    const auto result = future.get();
    EXPECT_TRUE(run);
    EXPECT_TRUE(result.first.empty()) << result.first;
    EXPECT_EQ(result.second.status, 503);
    if (!result.second.body.empty()) {
        EXPECT_EQ(
            nlohmann::json::parse(result.second.body)["error"]["code"],
            "queue_full");
    }
    EXPECT_TRUE(service->stop());
    service.reset();
}

struct ErrorCase {
    const char* name;
    std::string request;
    int expected_status;
};

class ServiceErrorMappingTest
    : public ::testing::TestWithParam<ErrorCase> {};

TEST_P(ServiceErrorMappingTest, ReturnsStableStatusOverLoopback) {
    const auto result = exchange_once(GetParam().request);
    EXPECT_TRUE(result.client_error.empty()) << result.client_error;
    EXPECT_EQ(result.response.status, GetParam().expected_status);
    if (!result.response.body.empty() &&
        result.response.body.front() == '{') {
        const auto body = nlohmann::json::parse(result.response.body);
        EXPECT_TRUE(body.contains("error"));
    }
}

INSTANTIATE_TEST_SUITE_P(
    ApiErrors,
    ServiceErrorMappingTest,
    ::testing::Values(
        ErrorCase{
            "malformed_json",
            post_request("{"),
            400},
        ErrorCase{
            "trailing_json",
            post_request("{}x"),
            400},
        ErrorCase{
            "array_root",
            post_request("[]"),
            400},
        ErrorCase{
            "missing_operation",
            post_request(R"({"input":{}})"),
            400},
        ErrorCase{
            "missing_input",
            post_request(R"({"operation":"echo"})"),
            400},
        ErrorCase{
            "extra_field",
            post_request(R"({"operation":"echo","input":{},"x":1})"),
            400},
        ErrorCase{
            "missing_content_type",
            post_request(
                R"({"operation":"echo","input":{"payload":1}})",
                ""),
            415},
        ErrorCase{
            "wrong_content_type",
            post_request("{}", "text/plain"),
            415},
        ErrorCase{
            "unknown_operation",
            post_request(R"({"operation":"unknown","input":{}})"),
            404},
        ErrorCase{
            "echo_validation",
            post_request(R"({"operation":"echo","input":{}})"),
            422},
        ErrorCase{
            "mock_validation",
            post_request(
                R"({"operation":"mock_vision.detect","input":{"image_id":"a","width":0,"height":1}})"),
            422},
        ErrorCase{
            "unknown_get",
            "GET /v1/tasks/task-0000000000000999 HTTP/1.1\r\n"
            "Host: localhost\r\nConnection: close\r\n\r\n",
            404},
        ErrorCase{
            "bad_id",
            "GET /v1/tasks/1 HTTP/1.1\r\n"
            "Host: localhost\r\nConnection: close\r\n\r\n",
            400},
        ErrorCase{
            "extra_segment",
            "GET /v1/tasks/task-0000000000000001/extra HTTP/1.1\r\n"
            "Host: localhost\r\nConnection: close\r\n\r\n",
            404},
        ErrorCase{
            "post_status_route",
            "POST /v1/tasks/task-0000000000000001 HTTP/1.1\r\n"
            "Host: localhost\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n",
            405},
        ErrorCase{
            "empty_id",
            "GET /v1/tasks/ HTTP/1.1\r\n"
            "Host: localhost\r\nConnection: close\r\n\r\n",
            404},
        ErrorCase{
            "leading_zero_id",
            "GET /v1/tasks/task-0000000000000000 HTTP/1.1\r\n"
            "Host: localhost\r\nConnection: close\r\n\r\n",
            400},
        ErrorCase{
            "short_task_id",
            "GET /v1/tasks/task-1 HTTP/1.1\r\n"
            "Host: localhost\r\nConnection: close\r\n\r\n",
            400},
        ErrorCase{
            "signed_task_id",
            "GET /v1/tasks/task-+000000000000001 HTTP/1.1\r\n"
            "Host: localhost\r\nConnection: close\r\n\r\n",
            400},
        ErrorCase{
            "task_collection_get",
            "GET /v1/tasks HTTP/1.1\r\n"
            "Host: localhost\r\nConnection: close\r\n\r\n",
            405},
        ErrorCase{
            "extra_media_parameter",
            post_request(
                R"({"operation":"echo","input":{"payload":1}})",
                "application/json; charset=utf-8; profile=x"),
            415},
        ErrorCase{
            "operation_number",
            post_request(R"({"operation":1,"input":{}})"),
            400},
        ErrorCase{
            "null_root",
            post_request("null"),
            400}),
    [](const ::testing::TestParamInfo<ErrorCase>& test_info) {
        return test_info.param.name;
    });

}  // namespace
}  // namespace iaisf::service
