#include "iaisf/http/http_session.hpp"

#include <new>
#include <string_view>
#include <utility>

namespace iaisf::http {

Result<HttpSession::Ptr> HttpSession::create(
    net::EventLoop& loop,
    const HttpRouter& router,
    HttpLimits limits,
    const ConnectionPtr& connection) {
    if (!loop.is_in_loop_thread()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidState,
            "HttpSession must be created in the EventLoop owner thread"));
    }
    if (!router.frozen() || !connection) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "HttpSession requires a frozen router and connection"));
    }
    try {
        return Result<Ptr>::success(std::shared_ptr<HttpSession>{
            new HttpSession{
                loop,
                router,
                std::move(limits),
                connection}});
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HttpSession allocation failed"));
    }
}

HttpSession::HttpSession(
    net::EventLoop& loop,
    const HttpRouter& router,
    HttpLimits limits,
    const ConnectionPtr& connection)
    : loop_(loop),
      router_(router),
      limits_(std::move(limits)),
      parser_(limits_),
      connection_(connection) {}

void HttpSession::on_message(
    const ConnectionPtr& connection,
    net::tcp::Buffer& input) noexcept {
    if (terminal_ || !connection || connection_.lock() != connection ||
        !loop_.is_in_loop_thread()) {
        if (connection) {
            close_without_response(connection);
        }
        return;
    }
    if (continuation_pending_) {
        return;
    }
    dispatch_available(connection, input);
}

void HttpSession::on_disconnected() noexcept {
    terminal_ = true;
    continuation_pending_ = false;
    continuation_input_ = nullptr;
    connection_.reset();
}

bool HttpSession::terminal() const noexcept {
    return terminal_;
}

bool HttpSession::continuation_pending() const noexcept {
    return continuation_pending_;
}

void HttpSession::dispatch_available(
    const ConnectionPtr& connection,
    net::tcp::Buffer& input) noexcept {
    std::size_t dispatched = 0U;
    while (!terminal_ &&
           dispatched < limits_.max_requests_per_dispatch() &&
           input.readable_bytes() > 0U) {
        const std::string_view bytes{
            input.peek(),
            input.readable_bytes()};
        auto progress_result = parser_.parse(bytes);
        if (!progress_result) {
            input.retrieve_all();
            send_error(connection, HttpStatus::InternalServerError);
            return;
        }
        const auto progress = progress_result.value();
        if (progress.consumed > 0U) {
            auto retrieve_result = input.retrieve(progress.consumed);
            if (!retrieve_result) {
                input.retrieve_all();
                close_without_response(connection);
                return;
            }
        }
        if (progress.disposition == ParseDisposition::NeedMore) {
            return;
        }
        if (progress.disposition == ParseDisposition::Error) {
            input.retrieve_all();
            send_error(connection, progress.error_status);
            return;
        }

        auto request_result = parser_.take_request();
        if (!request_result) {
            input.retrieve_all();
            send_error(connection, HttpStatus::InternalServerError);
            return;
        }
        auto request = std::move(request_result).value();
        auto response_result = router_.dispatch(request);
        if (!response_result) {
            input.retrieve_all();
            send_error(connection, HttpStatus::InternalServerError);
            return;
        }
        send_response(
            connection,
            std::move(response_result).value(),
            request.keep_alive());
        ++dispatched;
        if (terminal_) {
            input.retrieve_all();
            return;
        }
    }

    if (!terminal_ && input.readable_bytes() > 0U) {
        schedule_continuation(connection, input);
    }
}

void HttpSession::schedule_continuation(
    const ConnectionPtr& connection,
    net::tcp::Buffer& input) noexcept {
    if (continuation_pending_) {
        return;
    }
    continuation_pending_ = true;
    continuation_input_ = &input;
    try {
        const std::weak_ptr<HttpSession> weak_session = weak_from_this();
        const std::weak_ptr<net::tcp::TcpConnection> weak_connection =
            connection;
        net::tcp::Buffer* const pending_input = &input;
        auto queued = loop_.queue_in_loop(
            [weak_session, weak_connection, pending_input] {
                const auto session = weak_session.lock();
                const auto connection_ptr = weak_connection.lock();
                if (!session || !connection_ptr ||
                    session->continuation_input_ != pending_input) {
                    return;
                }
                session->continuation_pending_ = false;
                session->continuation_input_ = nullptr;
                if (!session->terminal_ &&
                    connection_ptr->state() ==
                        net::tcp::TcpConnection::State::Connected) {
                    session->dispatch_available(
                        connection_ptr,
                        *pending_input);
                }
            });
        if (!queued) {
            continuation_pending_ = false;
            continuation_input_ = nullptr;
            close_without_response(connection);
        }
    } catch (...) {
        continuation_pending_ = false;
        continuation_input_ = nullptr;
        close_without_response(connection);
    }
}

void HttpSession::send_response(
    const ConnectionPtr& connection,
    HttpResponse response,
    bool request_keep_alive) noexcept {
    if (!request_keep_alive) {
        response.set_close_connection(true);
    }
    const bool close_after_response = response.close_connection();
    auto serialized = response.serialize(limits_);
    if (!serialized) {
        send_error(connection, HttpStatus::InternalServerError);
        return;
    }
    auto send_result = connection->send(
        serialized.value().data(),
        serialized.value().size());
    if (!send_result) {
        close_without_response(connection);
        return;
    }
    if (close_after_response) {
        terminal_ = true;
        auto close_result = connection->close_after_write();
        if (!close_result) {
            const auto force_result = connection->force_close();
            if (!force_result) {
                terminal_ = true;
            }
        }
    }
}

void HttpSession::send_error(
    const ConnectionPtr& connection,
    HttpStatus status) noexcept {
    if (terminal_) {
        return;
    }
    HttpResponse response;
    try {
        response = HttpResponse::error(status, true);
    } catch (...) {
        close_without_response(connection);
        return;
    }
    auto serialized = response.serialize(limits_);
    if (!serialized) {
        close_without_response(connection);
        return;
    }
    auto send_result = connection->send(
        serialized.value().data(),
        serialized.value().size());
    if (!send_result) {
        close_without_response(connection);
        return;
    }
    terminal_ = true;
    auto close_result = connection->close_after_write();
    if (!close_result) {
        const auto force_result = connection->force_close();
        if (!force_result) {
            terminal_ = true;
        }
    }
}

void HttpSession::close_without_response(
    const ConnectionPtr& connection) noexcept {
    terminal_ = true;
    continuation_pending_ = false;
    continuation_input_ = nullptr;
    if (connection) {
        const auto close_result = connection->force_close();
        if (!close_result) {
            terminal_ = true;
        }
    }
}

}  // namespace iaisf::http
