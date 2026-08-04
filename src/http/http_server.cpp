#include "iaisf/http/http_server.hpp"

#include <exception>
#include <new>
#include <utility>

namespace iaisf::http {

Result<HttpServer::Ptr> HttpServer::create(
    net::EventLoop& loop,
    ILogger& logger,
    const net::tcp::Ipv4Endpoint& bind_endpoint,
    net::tcp::TcpServerOptions tcp_options,
    HttpRouter router,
    HttpLimits limits,
    std::optional<std::chrono::steady_clock::duration> header_timeout,
    std::optional<std::chrono::steady_clock::duration> body_timeout) {
    if (!loop.is_in_loop_thread()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidState,
            "HttpServer must be created in the EventLoop owner thread"));
    }
    if (!router.frozen()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HttpServer requires a frozen router"));
    }
    if (header_timeout.has_value() &&
        *header_timeout <= std::chrono::steady_clock::duration::zero()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP header timeout must be positive"));
    }
    if (body_timeout.has_value() &&
        *body_timeout <= std::chrono::steady_clock::duration::zero()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HTTP body timeout must be positive"));
    }

    const auto max_connections = tcp_options.max_connections();
    auto tcp_result = net::tcp::TcpServer::create(
        loop,
        logger,
        bind_endpoint,
        std::move(tcp_options));
    if (!tcp_result) {
        return Result<Ptr>::failure(std::move(tcp_result).error());
    }

    try {
        auto server = std::shared_ptr<HttpServer>{new HttpServer{
            loop,
            logger,
            std::move(limits),
            std::move(router),
            std::move(tcp_result).value(),
            header_timeout,
            body_timeout}};
        server->sessions_.reserve(max_connections);
        return Result<Ptr>::success(std::move(server));
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HttpServer allocation failed"));
    }
}

HttpServer::HttpServer(
    net::EventLoop& loop,
    ILogger& logger,
    HttpLimits limits,
    HttpRouter router,
    net::tcp::TcpServer::Ptr tcp_server,
    std::optional<std::chrono::steady_clock::duration> header_timeout,
    std::optional<std::chrono::steady_clock::duration> body_timeout) noexcept
    : loop_(loop),
      logger_(logger),
      limits_(std::move(limits)),
      router_(std::move(router)),
      tcp_server_(std::move(tcp_server)),
      header_timeout_(header_timeout),
      body_timeout_(body_timeout) {}

HttpServer::~HttpServer() noexcept {
    if ((state_ == State::Running || state_ == State::Stopping) &&
        !stopped()) {
        std::terminate();
    }
    if (!sessions_.empty()) {
        std::terminate();
    }
}

Result<void> HttpServer::start() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HttpServer::start must run in the EventLoop owner thread"));
    }
    if (state_ != State::Created) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HttpServer cannot be started in its current state"));
    }

    try {
        const std::weak_ptr<HttpServer> weak_server = weak_from_this();
        auto result = tcp_server_->start(
            [weak_server](
                const net::tcp::TcpConnection::Ptr& connection,
                net::tcp::Buffer& input) {
                const auto server = weak_server.lock();
                if (server) {
                    server->handle_message(connection, input);
                } else {
                    const auto close_result = connection->force_close();
                    if (!close_result) {
                        input.retrieve_all();
                    }
                }
            },
            [weak_server](
                const net::tcp::TcpConnection::Ptr& connection) {
                const auto server = weak_server.lock();
                if (server) {
                    server->handle_connection(connection);
                } else {
                    const auto close_result = connection->force_close();
                    if (!close_result) {
                        return;
                    }
                }
            });
        if (!result) {
            return result;
        }
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HttpServer callback allocation failed"));
    }
    state_ = State::Running;
    return Result<void>::success();
}

Result<void> HttpServer::stop() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HttpServer::stop must run in the EventLoop owner thread"));
    }
    if (state_ == State::Stopped) {
        return Result<void>::success();
    }
    state_ = State::Stopping;
    auto result = tcp_server_->stop();
    if (!result) {
        return result;
    }
    if (tcp_server_->stopped() && sessions_.empty()) {
        state_ = State::Stopped;
    }
    return Result<void>::success();
}

bool HttpServer::started() const noexcept {
    return state_ == State::Running;
}

bool HttpServer::stopped() const noexcept {
    return state_ == State::Stopped ||
           (state_ == State::Stopping &&
            tcp_server_->stopped() &&
            sessions_.empty());
}

std::size_t HttpServer::session_count() const noexcept {
    return sessions_.size();
}

std::size_t HttpServer::connection_count() const noexcept {
    return tcp_server_->connection_count();
}

const net::tcp::Ipv4Endpoint& HttpServer::local_endpoint() const noexcept {
    return tcp_server_->local_endpoint();
}

void HttpServer::handle_connection(
    const net::tcp::TcpConnection::Ptr& connection) noexcept {
    if (!connection) {
        return;
    }
    if (connection->state() == net::tcp::TcpConnection::State::Connected) {
        if (state_ != State::Running) {
            const auto close_result = connection->force_close();
            if (!close_result) {
                return;
            }
            return;
        }
        auto session = HttpSession::create(
            loop_,
            router_,
            limits_,
            connection,
            header_timeout_,
            body_timeout_);
        if (!session) {
            const auto close_result = connection->force_close();
            if (!close_result) {
                return;
            }
            return;
        }
        try {
            const auto inserted = sessions_.emplace(
                connection->id(),
                std::move(session).value());
            if (!inserted.second) {
                const auto close_result = connection->force_close();
                if (!close_result) {
                    return;
                }
            }
        } catch (...) {
            const auto close_result = connection->force_close();
            if (!close_result) {
                return;
            }
        }
        return;
    }

    const auto found = sessions_.find(connection->id());
    if (found != sessions_.end()) {
        found->second->on_disconnected();
        sessions_.erase(found);
    }
}

void HttpServer::handle_message(
    const net::tcp::TcpConnection::Ptr& connection,
    net::tcp::Buffer& input) noexcept {
    if (state_ != State::Running || !connection) {
        input.retrieve_all();
        if (connection) {
            const auto close_result = connection->force_close();
            if (!close_result) {
                return;
            }
        }
        return;
    }
    const auto found = sessions_.find(connection->id());
    if (found == sessions_.end()) {
        input.retrieve_all();
        const auto close_result = connection->force_close();
        if (!close_result) {
            return;
        }
        return;
    }
    found->second->on_message(connection, input);
}

}  // namespace iaisf::http
