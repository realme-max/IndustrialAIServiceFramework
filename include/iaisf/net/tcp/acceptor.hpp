#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/channel.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/socket.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"

namespace iaisf::net::tcp {

/**
 * Owner-thread-only edge-triggered IPv4 accept loop.
 *
 * Acceptor owns the listening Socket. Channel is declared after Socket so the
 * non-owning Channel is destroyed first. stop() is idempotent. When called
 * from an active Channel callback it disables logical acceptance immediately
 * and uses EventLoop's internal cleanup lane to remove the Channel afterward.
 */
class Acceptor final {
public:
    using NewConnectionCallback =
        std::function<void(Socket, const Ipv4Endpoint&)>;

    static constexpr int kMaximumBacklog = 65'535;

    [[nodiscard]] static Result<std::unique_ptr<Acceptor>> create(
        EventLoop& loop,
        ILogger& logger,
        const Ipv4Endpoint& bind_endpoint,
        int backlog);

    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    Acceptor(Acceptor&&) = delete;
    Acceptor& operator=(Acceptor&&) = delete;
    ~Acceptor() noexcept;

    [[nodiscard]] Result<void> start(NewConnectionCallback callback);
    [[nodiscard]] Result<void> stop();

    [[nodiscard]] bool listening() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] const Ipv4Endpoint& local_endpoint() const noexcept;
    [[nodiscard]] std::size_t accepted_count() const noexcept;
    [[nodiscard]] std::size_t logger_failure_count() const noexcept;

private:
    enum class State {
        Created,
        Listening,
        Stopping,
        Stopped,
    };

    Acceptor(
        EventLoop& loop,
        ILogger& logger,
        Socket listening_socket,
        Ipv4Endpoint local_endpoint,
        int backlog) noexcept;

    void handle_read();
    [[nodiscard]] Result<void> finish_stop();
    static void run_deferred_stop(void* context) noexcept;
    void safe_log(LogLevel level, std::string_view message) noexcept;

    EventLoop& loop_;
    ILogger& logger_;
    Socket listening_socket_;
    Channel channel_;
    Ipv4Endpoint local_endpoint_;
    int backlog_;
    State state_{State::Created};
    std::size_t accepted_count_{0U};
    std::size_t logger_failure_count_{0U};
    NewConnectionCallback new_connection_callback_;
    EventLoop::DeferredCleanup deferred_stop_;
};

}  // namespace iaisf::net::tcp
