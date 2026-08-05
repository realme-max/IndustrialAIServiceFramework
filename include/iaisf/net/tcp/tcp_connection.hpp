#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/channel.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/socket.hpp"
#include "iaisf/net/tcp/buffer.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"

namespace iaisf::net::tcp {

/**
 * Single-EventLoop TCP byte-stream connection.
 *
 * All methods except immutable getters are owner-thread-only. The server
 * connection table is the primary owner. Channel callbacks capture weak_ptr,
 * and destruction is legal only after connect_destroyed() removes the Channel.
 *
 * send() has an all-accepted-or-failure contract. Success means every input
 * byte is owned by the output Buffer; failure occurs before any byte from that
 * call is sent or appended. Actual nonblocking send syscalls run from the
 * writable callback, where a partial kernel write always leaves its suffix in
 * the already-reserved Buffer.
 */
class TcpConnection final
    : public std::enable_shared_from_this<TcpConnection> {
public:
    using Ptr = std::shared_ptr<TcpConnection>;
    using ConnectionCallback = std::function<void(const Ptr&)>;
    using MessageCallback = std::function<void(const Ptr&, Buffer&)>;
    using CloseCallback = std::function<void(const Ptr&)>;
    using HighWaterCallback =
        std::function<void(const Ptr&, std::size_t)>;

    enum class State {
        Connecting,
        Connected,
        Disconnecting,
        Disconnected,
    };

    static constexpr std::size_t kMaximumBufferBytes =
        64U * 1024U * 1024U;

    [[nodiscard]] static Result<Ptr> create(
        EventLoop& loop,
        ILogger& logger,
        std::uint64_t connection_id,
        Socket socket,
        Ipv4Endpoint local_endpoint,
        Ipv4Endpoint peer_endpoint,
        std::size_t input_initial_capacity,
        std::size_t input_maximum_capacity,
        std::size_t output_initial_capacity,
        std::size_t output_maximum_capacity,
        std::size_t output_high_water_mark,
        std::optional<std::chrono::steady_clock::duration> idle_timeout =
            std::nullopt,
        MetricsRegistry* metrics = nullptr);

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&) = delete;
    TcpConnection& operator=(TcpConnection&&) = delete;
    ~TcpConnection() noexcept;

    [[nodiscard]] Result<void> set_connection_callback(
        ConnectionCallback callback);
    [[nodiscard]] Result<void> set_message_callback(MessageCallback callback);
    [[nodiscard]] Result<void> set_close_callback(CloseCallback callback);
    [[nodiscard]] Result<void> set_high_water_callback(
        HighWaterCallback callback);

    [[nodiscard]] Result<void> connect_established();
    [[nodiscard]] Result<void> connect_destroyed();
    [[nodiscard]] Result<void> send(const void* data, std::size_t length);
    [[nodiscard]] Result<void> send(std::string_view bytes);
    [[nodiscard]] Result<void> shutdown();
    /**
     * Flushes already accepted output and then fully closes the connection.
     *
     * This owner-thread-only operation is idempotent. It immediately rejects
     * later send() calls, disables further input, and does not wait for peer
     * EOF. The existing shutdown() API retains its graceful half-close
     * contract.
     */
    [[nodiscard]] Result<void> close_after_write();
    [[nodiscard]] Result<void> force_close();

    [[nodiscard]] std::uint64_t id() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] const Ipv4Endpoint& local_endpoint() const noexcept;
    [[nodiscard]] const Ipv4Endpoint& peer_endpoint() const noexcept;
    [[nodiscard]] std::size_t input_readable_bytes() const noexcept;
    [[nodiscard]] std::size_t output_readable_bytes() const noexcept;
    [[nodiscard]] bool writing_enabled() const noexcept;
    [[nodiscard]] bool peer_eof_received() const noexcept;
    [[nodiscard]] std::size_t logger_failure_count() const noexcept;

private:
    friend struct TcpConnectionTestAccess;

    TcpConnection(
        EventLoop& loop,
        ILogger& logger,
        std::uint64_t connection_id,
        Socket socket,
        Ipv4Endpoint local_endpoint,
        Ipv4Endpoint peer_endpoint,
        Buffer input_buffer,
        Buffer output_buffer,
        std::size_t output_high_water_mark,
        std::optional<std::chrono::steady_clock::duration> idle_timeout,
        MetricsRegistry* metrics)
        noexcept;

    void initialize_metrics() noexcept;
    void handle_read();
    void handle_write();
    void handle_error();
    void handle_close();
    [[nodiscard]] Result<void> refresh_idle_timeout();
    void cancel_idle_timeout() noexcept;
    void handle_idle_timeout(std::uint64_t generation) noexcept;
    [[nodiscard]] Result<void> finish_write_shutdown();
    void begin_close();
    void fail_connection(const Error& error);
    void notify_high_water_if_crossed(std::size_t previous_size);
    void rearm_high_water_if_below() noexcept;
    void safe_log(LogLevel level, std::string_view message) noexcept;

    EventLoop& loop_;
    ILogger& logger_;
    const std::uint64_t connection_id_;
    Socket socket_;
    Channel channel_;
    Ipv4Endpoint local_endpoint_;
    Ipv4Endpoint peer_endpoint_;
    Buffer input_buffer_;
    Buffer output_buffer_;
    const std::size_t output_high_water_mark_;
    const std::optional<std::chrono::steady_clock::duration> idle_timeout_;
    MetricsRegistry* const metrics_{nullptr};
    std::shared_ptr<Counter> closed_metric_;
    std::shared_ptr<Counter> idle_timeout_metric_;
    std::shared_ptr<Gauge> active_metric_;

    State state_{State::Connecting};
    std::optional<TimerId> idle_timer_;
    std::uint64_t idle_generation_{0U};
    bool peer_eof_received_{false};
    bool shutdown_requested_{false};
    bool close_after_write_requested_{false};
    bool write_shutdown_{false};
    bool close_notified_{false};
    bool active_metric_counted_{false};
    bool high_water_above_{false};
    std::size_t logger_failure_count_{0U};

    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    CloseCallback close_callback_;
    HighWaterCallback high_water_callback_;
};

}  // namespace iaisf::net::tcp
