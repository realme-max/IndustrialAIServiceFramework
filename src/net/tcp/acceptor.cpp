#include "iaisf/net/tcp/acceptor.hpp"

#include <exception>
#include <new>
#include <string_view>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::net::tcp {

Result<std::unique_ptr<Acceptor>> Acceptor::create(
    EventLoop& loop,
    ILogger& logger,
    const Ipv4Endpoint& bind_endpoint,
    const int backlog) {
    if (!loop.is_in_loop_thread()) {
        return Result<std::unique_ptr<Acceptor>>::failure(make_error(
            ErrorCode::InvalidState,
            "Acceptor must be created in the EventLoop owner thread"));
    }
    if (backlog <= 0 || backlog > kMaximumBacklog) {
        return Result<std::unique_ptr<Acceptor>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "Acceptor backlog is outside the supported range"));
    }

    auto socket_result = Socket::create_ipv4_tcp();
    if (!socket_result) {
        return Result<std::unique_ptr<Acceptor>>::failure(
            socket_result.error());
    }
    Socket listening_socket = std::move(socket_result).value();
    auto reuse_result = listening_socket.set_reuse_address();
    if (!reuse_result) {
        return Result<std::unique_ptr<Acceptor>>::failure(
            reuse_result.error());
    }
    auto bind_result = listening_socket.bind(bind_endpoint);
    if (!bind_result) {
        return Result<std::unique_ptr<Acceptor>>::failure(
            bind_result.error());
    }
    auto endpoint_result = listening_socket.local_endpoint();
    if (!endpoint_result) {
        return Result<std::unique_ptr<Acceptor>>::failure(
            endpoint_result.error());
    }

    try {
        return Result<std::unique_ptr<Acceptor>>::success(
            std::unique_ptr<Acceptor>{new Acceptor{
                loop,
                logger,
                std::move(listening_socket),
                std::move(endpoint_result).value(),
                backlog}});
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<Acceptor>>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Acceptor allocation failed"));
    }
}

Acceptor::Acceptor(
    EventLoop& loop,
    ILogger& logger,
    Socket listening_socket,
    Ipv4Endpoint local_endpoint,
    const int backlog) noexcept
    : loop_(loop),
      logger_(logger),
      listening_socket_(std::move(listening_socket)),
      channel_(loop_, listening_socket_.native_handle()),
      local_endpoint_(std::move(local_endpoint)),
      backlog_(backlog),
      deferred_stop_(this, &Acceptor::run_deferred_stop) {}

Acceptor::~Acceptor() noexcept {
    if (!loop_.is_in_loop_thread() ||
        state_ == State::Listening ||
        state_ == State::Stopping ||
        channel_.is_registered() ||
        deferred_stop_.pending()) {
        std::terminate();
    }
}

Result<void> Acceptor::start(NewConnectionCallback callback) {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "Acceptor::start must run in the EventLoop owner thread"));
    }
    if (state_ != State::Created) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "Acceptor can only be started once"));
    }
    if (!callback) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "Acceptor new-connection callback must not be empty"));
    }

    auto listen_result = listening_socket_.listen(backlog_);
    if (!listen_result) {
        return listen_result;
    }

    try {
        new_connection_callback_ = std::move(callback);
        channel_.set_read_callback([this] { handle_read(); });
        channel_.set_error_callback([this] {
            safe_log(LogLevel::Error, "listening socket reported EPOLLERR");
            handle_read();
        });
    } catch (const std::bad_alloc&) {
        new_connection_callback_ = {};
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "Acceptor callback allocation failed"));
    }

    channel_.enable_reading();
    channel_.set_edge_triggered(true);
    auto update_result = channel_.update();
    if (!update_result) {
        channel_.disable_all();
        new_connection_callback_ = {};
        return update_result;
    }
    state_ = State::Listening;
    return Result<void>::success();
}

Result<void> Acceptor::stop() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "Acceptor::stop must run in the EventLoop owner thread"));
    }
    if (state_ == State::Stopped) {
        return Result<void>::success();
    }
    if (state_ == State::Stopping) {
        if (deferred_stop_.pending()) {
            return Result<void>::success();
        }
        if (loop_.dispatching_active_channels()) {
            return loop_.defer_cleanup(deferred_stop_);
        }
        return finish_stop();
    }
    if (state_ == State::Listening &&
        loop_.dispatching_active_channels()) {
        state_ = State::Stopping;
        auto defer_result = loop_.defer_cleanup(deferred_stop_);
        if (!defer_result) {
            state_ = State::Listening;
            return defer_result;
        }
        return Result<void>::success();
    }
    state_ = State::Stopping;
    return finish_stop();
}

bool Acceptor::listening() const noexcept {
    return state_ == State::Listening;
}

bool Acceptor::stopped() const noexcept {
    return state_ == State::Stopped;
}

const Ipv4Endpoint& Acceptor::local_endpoint() const noexcept {
    return local_endpoint_;
}

std::size_t Acceptor::accepted_count() const noexcept {
    return accepted_count_;
}

std::size_t Acceptor::logger_failure_count() const noexcept {
    return logger_failure_count_;
}

void Acceptor::handle_read() {
    if (state_ != State::Listening) {
        return;
    }

    for (;;) {
        auto accept_result = accept_ipv4(listening_socket_);
        if (!accept_result) {
            safe_log(LogLevel::Error, accept_result.error().message);
            return;
        }
        auto accepted = std::move(accept_result).value();
        if (!accepted.has_value()) {
            return;
        }

        ++accepted_count_;
        try {
            new_connection_callback_(
                std::move(accepted->socket),
                accepted->peer_endpoint);
        } catch (const std::exception& exception) {
            safe_log(LogLevel::Error, exception.what());
        } catch (...) {
            safe_log(
                LogLevel::Error,
                "new-connection callback threw an unknown exception");
        }
        if (state_ != State::Listening) {
            return;
        }
    }
}

Result<void> Acceptor::finish_stop() {
    if (channel_.is_registered()) {
        auto remove_result = channel_.remove();
        if (!remove_result) {
            return remove_result;
        }
    }

    channel_.disable_all();
    new_connection_callback_ = {};
    listening_socket_.reset();
    state_ = State::Stopped;
    return Result<void>::success();
}

void Acceptor::run_deferred_stop(void* const context) noexcept {
    auto& acceptor = *static_cast<Acceptor*>(context);
    auto result = acceptor.finish_stop();
    if (!result) {
        acceptor.safe_log(LogLevel::Error, result.error().message);
        acceptor.loop_.stop();
    }
}

void Acceptor::safe_log(
    const LogLevel level,
    const std::string_view message) noexcept {
    try {
        logger_.log(level, "Acceptor", message);
    } catch (...) {
        ++logger_failure_count_;
    }
}

}  // namespace iaisf::net::tcp
