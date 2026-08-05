#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/socket.hpp"
#include "iaisf/net/tcp/acceptor.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/tcp/tcp_connection.hpp"

namespace iaisf::net::tcp {

/**
 * Validated hard limits for the single-Reactor TCP server.
 *
 * Signed factory inputs reject negative configuration before conversion to
 * size_t. Values above the public hard bounds fail instead of being clamped.
 */
class TcpServerOptions final {
public:
    static constexpr std::int64_t kMaximumConnections = 1'000'000;
    static constexpr std::int64_t kMaximumBufferBytes =
        64LL * 1024LL * 1024LL;
    static constexpr std::int64_t kMaximumIdleTimeoutMilliseconds =
        24LL * 60LL * 60LL * 1000LL;

    [[nodiscard]] static Result<TcpServerOptions> create(
        std::int64_t listen_backlog,
        std::int64_t max_connections,
        std::int64_t input_initial_capacity,
        std::int64_t input_maximum_capacity,
        std::int64_t output_initial_capacity,
        std::int64_t output_high_water_mark,
        std::int64_t output_maximum_capacity,
        std::optional<std::int64_t> socket_send_buffer_bytes = std::nullopt,
        std::optional<std::int64_t> idle_timeout_ms = std::nullopt);
    [[nodiscard]] static TcpServerOptions defaults() noexcept;

    [[nodiscard]] int listen_backlog() const noexcept;
    [[nodiscard]] std::size_t max_connections() const noexcept;
    [[nodiscard]] std::size_t input_initial_capacity() const noexcept;
    [[nodiscard]] std::size_t input_maximum_capacity() const noexcept;
    [[nodiscard]] std::size_t output_initial_capacity() const noexcept;
    [[nodiscard]] std::size_t output_high_water_mark() const noexcept;
    [[nodiscard]] std::size_t output_maximum_capacity() const noexcept;
    [[nodiscard]] std::optional<int> socket_send_buffer_bytes() const noexcept;
    [[nodiscard]] std::optional<std::chrono::milliseconds> idle_timeout()
        const noexcept;

private:
    TcpServerOptions(
        int listen_backlog,
        std::size_t max_connections,
        std::size_t input_initial_capacity,
        std::size_t input_maximum_capacity,
        std::size_t output_initial_capacity,
        std::size_t output_high_water_mark,
        std::size_t output_maximum_capacity,
        std::optional<int> socket_send_buffer_bytes,
        std::optional<std::chrono::milliseconds> idle_timeout) noexcept;

    int listen_backlog_;
    std::size_t max_connections_;
    std::size_t input_initial_capacity_;
    std::size_t input_maximum_capacity_;
    std::size_t output_initial_capacity_;
    std::size_t output_high_water_mark_;
    std::size_t output_maximum_capacity_;
    std::optional<int> socket_send_buffer_bytes_;
    std::optional<std::chrono::milliseconds> idle_timeout_;
};

/**
 * Owner-thread-only TCP connection table and listening service.
 *
 * TcpServer is shared-owned. Connection close callbacks capture a weak server
 * reference; accepted cleanup work is retained in preallocated shared_ptr
 * vectors and one embedded EventLoop::DeferredCleanup node. A started server
 * must reach stopped()==true before destruction, and the server must still be
 * destroyed before its EventLoop and injected logger.
 */
class TcpServer final : public std::enable_shared_from_this<TcpServer> {
public:
    using Ptr = std::shared_ptr<TcpServer>;
    using ConnectionPtr = TcpConnection::Ptr;
    using ConnectionCallback = TcpConnection::ConnectionCallback;
    using MessageCallback = TcpConnection::MessageCallback;
    using HighWaterCallback = TcpConnection::HighWaterCallback;

    [[nodiscard]] static Result<Ptr> create(
        EventLoop& loop,
        ILogger& logger,
        const Ipv4Endpoint& bind_endpoint,
        TcpServerOptions options,
        MetricsRegistry* metrics = nullptr);

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;
    ~TcpServer() noexcept;

    [[nodiscard]] Result<void> start(
        MessageCallback message_callback,
        ConnectionCallback connection_callback = {},
        HighWaterCallback high_water_callback = {});

    /**
     * Permanently stops acceptance and force-closes every current connection.
     *
     * The method is owner-thread-only and idempotent; start() is never allowed
     * afterward. Outside active Channel dispatch, success guarantees an empty
     * connection table and stopped()==true on return. Inside an active batch,
     * success accepts an asynchronous stop: disconnect notifications and table
     * removal run through the internal cleanup lane after the batch, and
     * stopped() becomes true only when that work is complete.
     */
    [[nodiscard]] Result<void> stop();

    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] const Ipv4Endpoint& local_endpoint() const noexcept;
    [[nodiscard]] std::size_t connection_count() const noexcept;
    [[nodiscard]] std::size_t rejected_connection_count() const noexcept;
    [[nodiscard]] std::size_t logger_failure_count() const noexcept;

private:
    enum class State {
        Created,
        Running,
        Stopping,
        Stopped,
    };

    TcpServer(
        EventLoop& loop,
        ILogger& logger,
        TcpServerOptions options,
        std::unique_ptr<Acceptor> acceptor,
        MetricsRegistry* metrics) noexcept;

    void initialize_metrics() noexcept;

    void handle_new_connection(Socket socket, const Ipv4Endpoint& peer);
    void schedule_remove_connection(const ConnectionPtr& connection) noexcept;
    void remove_connection(const ConnectionPtr& connection) noexcept;
    void drain_pending_removals() noexcept;
    void complete_stop_if_ready() noexcept;
    static void run_deferred_cleanup(void* context) noexcept;
    void safe_log(LogLevel level, std::string_view message) noexcept;

    EventLoop& loop_;
    ILogger& logger_;
    TcpServerOptions options_;
    std::unique_ptr<Acceptor> acceptor_;
    MetricsRegistry* const metrics_{nullptr};
    std::shared_ptr<Counter> accepted_metric_;
    std::unordered_map<std::uint64_t, ConnectionPtr> connections_;
    std::vector<ConnectionPtr> pending_removals_;
    std::vector<ConnectionPtr> stop_snapshot_;
    EventLoop::DeferredCleanup deferred_cleanup_;
    std::uint64_t next_connection_id_{1U};
    std::size_t rejected_connection_count_{0U};
    std::size_t logger_failure_count_{0U};
    State state_{State::Created};
    bool draining_removals_{false};
    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    HighWaterCallback high_water_callback_;
};

}  // namespace iaisf::net::tcp
