#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_parser.hpp"
#include "iaisf/http/http_router.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/buffer.hpp"
#include "iaisf/net/tcp/tcp_connection.hpp"

namespace iaisf::http {

class HttpSessionTestAccess;

/**
 * Owner-thread-only HTTP protocol state for one TCP connection.
 *
 * The session is owned by HttpServer, does not own the TcpConnection, and
 * retains no string_view into the TCP Buffer. A bounded number of complete
 * requests is dispatched per callback; remaining pipelined input is resumed
 * through EventLoop::queue_in_loop().
 */
class HttpSession final
    : public std::enable_shared_from_this<HttpSession> {
public:
    using Ptr = std::shared_ptr<HttpSession>;
    using ConnectionPtr = net::tcp::TcpConnection::Ptr;

    [[nodiscard]] static Result<Ptr> create(
        net::EventLoop& loop,
        const HttpRouter& router,
        HttpLimits limits,
        const ConnectionPtr& connection,
        std::optional<std::chrono::steady_clock::duration> header_timeout =
            std::nullopt,
        std::optional<std::chrono::steady_clock::duration> body_timeout =
            std::nullopt);

    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;
    HttpSession(HttpSession&&) = delete;
    HttpSession& operator=(HttpSession&&) = delete;
    ~HttpSession() = default;

    void on_message(
        const ConnectionPtr& connection,
        net::tcp::Buffer& input) noexcept;
    void on_disconnected() noexcept;

    [[nodiscard]] bool terminal() const noexcept;
    [[nodiscard]] bool continuation_pending() const noexcept;

private:
    enum class ReceiveState {
        AwaitingRequest,
        ReadingHeaders,
        ReadingBody,
        AfterHeaders,
        Terminal,
    };

    HttpSession(
        net::EventLoop& loop,
        const HttpRouter& router,
        HttpLimits limits,
        const ConnectionPtr& connection,
        std::optional<std::chrono::steady_clock::duration> header_timeout,
        std::optional<std::chrono::steady_clock::duration> body_timeout);

    void dispatch_available(
        const ConnectionPtr& connection,
        net::tcp::Buffer& input) noexcept;
    void schedule_continuation(
        const ConnectionPtr& connection,
        net::tcp::Buffer& input) noexcept;
    void send_response(
        const ConnectionPtr& connection,
        HttpResponse response,
        bool request_keep_alive) noexcept;
    void send_error(
        const ConnectionPtr& connection,
        HttpStatus status) noexcept;
    void close_without_response(const ConnectionPtr& connection) noexcept;
    [[nodiscard]] Result<void> begin_header_timeout();
    void cancel_header_timeout() noexcept;
    void handle_header_timeout(std::uint64_t generation) noexcept;
    [[nodiscard]] Result<void> begin_body_timeout();
    [[nodiscard]] Result<void> refresh_body_timeout();
    void cancel_body_timeout() noexcept;
    void handle_body_timeout(std::uint64_t generation) noexcept;

    net::EventLoop& loop_;
    const HttpRouter& router_;
    HttpLimits limits_;
    HttpParser parser_;
    std::weak_ptr<net::tcp::TcpConnection> connection_;
    std::optional<std::chrono::steady_clock::duration> header_timeout_;
    std::optional<net::TimerId> header_timer_;
    std::uint64_t header_generation_{0U};
    std::optional<std::chrono::steady_clock::duration> body_timeout_;
    std::optional<net::TimerId> body_timer_;
    std::uint64_t body_generation_{0U};
    net::tcp::Buffer* continuation_input_{nullptr};
    bool continuation_pending_{false};
    bool terminal_{false};
    ReceiveState receive_state_{ReceiveState::AwaitingRequest};

    friend class HttpSessionTestAccess;
};

}  // namespace iaisf::http
