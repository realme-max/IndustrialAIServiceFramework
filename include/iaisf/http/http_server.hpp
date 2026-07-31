#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "iaisf/core/result.hpp"
#include "iaisf/http/http_limits.hpp"
#include "iaisf/http/http_router.hpp"
#include "iaisf/http/http_session.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/event_loop.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/tcp/tcp_server.hpp"

namespace iaisf::http {

/**
 * Owner-thread-only HTTP adapter over TcpServer.
 *
 * HttpServer owns a frozen router, the TcpServer and a bounded session table,
 * but not EventLoop or ILogger. Tcp callbacks capture only weak server
 * references. A started server must complete stop before destruction.
 */
class HttpServer final : public std::enable_shared_from_this<HttpServer> {
public:
    using Ptr = std::shared_ptr<HttpServer>;

    [[nodiscard]] static Result<Ptr> create(
        net::EventLoop& loop,
        ILogger& logger,
        const net::tcp::Ipv4Endpoint& bind_endpoint,
        net::tcp::TcpServerOptions tcp_options,
        HttpRouter router,
        HttpLimits limits = HttpLimits::defaults());

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;
    ~HttpServer() noexcept;

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> stop();

    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] std::size_t session_count() const noexcept;
    [[nodiscard]] std::size_t connection_count() const noexcept;
    [[nodiscard]] const net::tcp::Ipv4Endpoint& local_endpoint() const noexcept;

private:
    enum class State {
        Created,
        Running,
        Stopping,
        Stopped,
    };

    HttpServer(
        net::EventLoop& loop,
        ILogger& logger,
        HttpLimits limits,
        HttpRouter router,
        net::tcp::TcpServer::Ptr tcp_server) noexcept;

    void handle_connection(
        const net::tcp::TcpConnection::Ptr& connection) noexcept;
    void handle_message(
        const net::tcp::TcpConnection::Ptr& connection,
        net::tcp::Buffer& input) noexcept;

    net::EventLoop& loop_;
    ILogger& logger_;
    HttpLimits limits_;
    HttpRouter router_;
    net::tcp::TcpServer::Ptr tcp_server_;
    std::unordered_map<std::uint64_t, HttpSession::Ptr> sessions_;
    State state_{State::Created};
};

}  // namespace iaisf::http
