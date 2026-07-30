#include "iaisf/net/tcp/tcp_connection.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <exception>
#include <limits>
#include <new>
#include <utility>

#include <sys/socket.h>
#include <unistd.h>

#include "../system_error.hpp"
#include "iaisf/core/error.hpp"

namespace iaisf::net::tcp {
namespace {

constexpr std::size_t kReadChunkSize = 64U * 1024U;

}  // namespace

Result<TcpConnection::Ptr> TcpConnection::create(
    EventLoop& loop,
    ILogger& logger,
    const std::uint64_t connection_id,
    Socket socket,
    Ipv4Endpoint local_endpoint,
    Ipv4Endpoint peer_endpoint,
    const std::size_t input_initial_capacity,
    const std::size_t input_maximum_capacity,
    const std::size_t output_initial_capacity,
    const std::size_t output_maximum_capacity,
    const std::size_t output_high_water_mark) {
    if (!loop.is_in_loop_thread()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection must be created in the EventLoop owner thread"));
    }
    if (!socket.valid()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TcpConnection requires a valid Socket"));
    }
    if (connection_id == 0U) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TcpConnection id must be nonzero"));
    }
    if (input_initial_capacity == 0U ||
        input_initial_capacity > input_maximum_capacity ||
        output_initial_capacity == 0U ||
        output_initial_capacity > output_maximum_capacity ||
        output_high_water_mark == 0U ||
        output_high_water_mark > output_maximum_capacity ||
        input_maximum_capacity > kMaximumBufferBytes ||
        output_maximum_capacity > kMaximumBufferBytes) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TcpConnection buffer limits are inconsistent"));
    }

    auto input_result =
        Buffer::create(input_initial_capacity, input_maximum_capacity);
    if (!input_result) {
        return Result<Ptr>::failure(input_result.error());
    }
    auto output_result =
        Buffer::create(output_initial_capacity, output_maximum_capacity);
    if (!output_result) {
        return Result<Ptr>::failure(output_result.error());
    }

    try {
        return Result<Ptr>::success(Ptr{new TcpConnection{
            loop,
            logger,
            connection_id,
            std::move(socket),
            std::move(local_endpoint),
            std::move(peer_endpoint),
            std::move(input_result).value(),
            std::move(output_result).value(),
            output_high_water_mark}});
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "TcpConnection allocation failed"));
    }
}

TcpConnection::TcpConnection(
    EventLoop& loop,
    ILogger& logger,
    const std::uint64_t connection_id,
    Socket socket,
    Ipv4Endpoint local_endpoint,
    Ipv4Endpoint peer_endpoint,
    Buffer input_buffer,
    Buffer output_buffer,
    const std::size_t output_high_water_mark) noexcept
    : loop_(loop),
      logger_(logger),
      connection_id_(connection_id),
      socket_(std::move(socket)),
      channel_(loop_, socket_.native_handle()),
      local_endpoint_(std::move(local_endpoint)),
      peer_endpoint_(std::move(peer_endpoint)),
      input_buffer_(std::move(input_buffer)),
      output_buffer_(std::move(output_buffer)),
      output_high_water_mark_(output_high_water_mark) {}

TcpConnection::~TcpConnection() noexcept {
    if (channel_.is_registered()) {
        std::terminate();
    }
}

Result<void> TcpConnection::set_connection_callback(
    ConnectionCallback callback) {
    if (!loop_.is_in_loop_thread() || state_ != State::Connecting) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "connection callback must be set before establishment"));
    }
    try {
        connection_callback_ = std::move(callback);
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "connection callback allocation failed"));
    }
}

Result<void> TcpConnection::set_message_callback(MessageCallback callback) {
    if (!loop_.is_in_loop_thread() || state_ != State::Connecting) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "message callback must be set before establishment"));
    }
    if (!callback) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "message callback must not be empty"));
    }
    try {
        message_callback_ = std::move(callback);
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "message callback allocation failed"));
    }
}

Result<void> TcpConnection::set_close_callback(CloseCallback callback) {
    if (!loop_.is_in_loop_thread() || state_ != State::Connecting) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "close callback must be set before establishment"));
    }
    if (!callback) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "close callback must not be empty"));
    }
    try {
        close_callback_ = std::move(callback);
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "close callback allocation failed"));
    }
}

Result<void> TcpConnection::set_high_water_callback(
    HighWaterCallback callback) {
    if (!loop_.is_in_loop_thread() || state_ != State::Connecting) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "high-water callback must be set before establishment"));
    }
    try {
        high_water_callback_ = std::move(callback);
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "high-water callback allocation failed"));
    }
}

Result<void> TcpConnection::connect_established() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "connect_established must run in the EventLoop owner thread"));
    }
    if (state_ != State::Connecting) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection is not in Connecting state"));
    }
    if (!message_callback_ || !close_callback_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection callbacks are incomplete"));
    }

    const std::weak_ptr<TcpConnection> weak_self = weak_from_this();
    try {
        channel_.set_read_callback([weak_self] {
            if (const auto self = weak_self.lock()) {
                self->handle_read();
            }
        });
        channel_.set_write_callback([weak_self] {
            if (const auto self = weak_self.lock()) {
                self->handle_write();
            }
        });
        channel_.set_error_callback([weak_self] {
            if (const auto self = weak_self.lock()) {
                self->handle_error();
            }
        });
        channel_.set_close_callback([weak_self] {
            if (const auto self = weak_self.lock()) {
                self->handle_close();
            }
        });
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "TcpConnection Channel callback allocation failed"));
    }

    channel_.enable_reading();
    channel_.set_edge_triggered(true);
    auto update_result = channel_.update();
    if (!update_result) {
        channel_.disable_all();
        return update_result;
    }
    state_ = State::Connected;

    if (connection_callback_) {
        try {
            connection_callback_(shared_from_this());
        } catch (const std::exception& exception) {
            safe_log(LogLevel::Error, exception.what());
            begin_close();
        } catch (...) {
            safe_log(
                LogLevel::Error,
                "connection callback threw an unknown exception");
            begin_close();
        }
    }
    return Result<void>::success();
}

Result<void> TcpConnection::connect_destroyed() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "connect_destroyed must run in the EventLoop owner thread"));
    }
    if (state_ == State::Disconnected) {
        return Result<void>::success();
    }
    const bool was_established = state_ != State::Connecting;
    if (channel_.is_registered()) {
        auto remove_result = channel_.remove();
        if (!remove_result) {
            return remove_result;
        }
    }

    channel_.disable_all();
    state_ = State::Disconnected;
    socket_.reset();
    const Ptr self = shared_from_this();
    if (was_established && connection_callback_) {
        try {
            connection_callback_(self);
        } catch (const std::exception& exception) {
            safe_log(LogLevel::Error, exception.what());
        } catch (...) {
            safe_log(
                LogLevel::Error,
                "disconnect callback threw an unknown exception");
        }
    }
    close_callback_ = {};
    return Result<void>::success();
}

Result<void> TcpConnection::send(
    const void* const data,
    const std::size_t length) {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection::send must run in the EventLoop owner thread"));
    }
    if (state_ != State::Connected || shutdown_requested_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection does not accept new output"));
    }
    if (length == 0U) {
        return Result<void>::success();
    }
    if (data == nullptr) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TcpConnection::send data must not be null"));
    }

    // Reserve the complete payload before enabling any path that can write it.
    // This is the acceptance boundary: capacity/allocation failure leaves the
    // existing readable bytes unchanged and no prefix reaches the peer.
    auto reserve_result = output_buffer_.ensure_writable(length);
    if (!reserve_result) {
        const Error error = reserve_result.error();
        fail_connection(error);
        return Result<void>::failure(error);
    }

    if (!writing_enabled()) {
        channel_.enable_writing();
        auto update_result = channel_.update();
        if (!update_result) {
            channel_.disable_writing();
            const Error error = update_result.error();
            fail_connection(error);
            return Result<void>::failure(error);
        }
    }

    const auto* const bytes = static_cast<const char*>(data);
    const std::size_t previous_size = output_buffer_.readable_bytes();
    auto append_result = output_buffer_.append(bytes, length);
    if (!append_result) {
        // ensure_writable() reserved this exact append. Reaching this branch
        // would violate the all-accepted-or-failure invariant.
        safe_log(LogLevel::Error, append_result.error().message);
        std::terminate();
    }
    notify_high_water_if_crossed(previous_size);
    return Result<void>::success();
}

Result<void> TcpConnection::send(const std::string_view bytes) {
    return send(bytes.data(), bytes.size());
}

Result<void> TcpConnection::shutdown() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection::shutdown must run in the EventLoop owner thread"));
    }
    if (state_ == State::Disconnected) {
        return Result<void>::success();
    }
    if (state_ != State::Connected && state_ != State::Disconnecting) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection cannot be shut down in its current state"));
    }

    shutdown_requested_ = true;
    state_ = State::Disconnecting;
    if (output_buffer_.empty()) {
        auto shutdown_result = finish_write_shutdown();
        if (!shutdown_result) {
            const Error error = shutdown_result.error();
            fail_connection(error);
            return shutdown_result;
        }
    }
    if (peer_eof_received_ && output_buffer_.empty()) {
        begin_close();
    }
    return Result<void>::success();
}

Result<void> TcpConnection::close_after_write() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection::close_after_write must run in the EventLoop owner thread"));
    }
    if (state_ == State::Disconnected || close_notified_ ||
        close_after_write_requested_) {
        return Result<void>::success();
    }
    if (state_ != State::Connected && state_ != State::Disconnecting) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection cannot close after write in its current state"));
    }

    close_after_write_requested_ = true;
    state_ = State::Disconnecting;
    if (output_buffer_.empty()) {
        begin_close();
        return Result<void>::success();
    }

    channel_.disable_reading();
    auto update_result = channel_.update();
    if (!update_result) {
        const Error error = update_result.error();
        fail_connection(error);
        return Result<void>::failure(error);
    }
    return Result<void>::success();
}

Result<void> TcpConnection::force_close() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpConnection::force_close must run in the EventLoop owner thread"));
    }
    if (state_ == State::Disconnected) {
        return Result<void>::success();
    }
    begin_close();
    return Result<void>::success();
}

std::uint64_t TcpConnection::id() const noexcept {
    return connection_id_;
}

TcpConnection::State TcpConnection::state() const noexcept {
    return state_;
}

bool TcpConnection::connected() const noexcept {
    return state_ == State::Connected;
}

const Ipv4Endpoint& TcpConnection::local_endpoint() const noexcept {
    return local_endpoint_;
}

const Ipv4Endpoint& TcpConnection::peer_endpoint() const noexcept {
    return peer_endpoint_;
}

std::size_t TcpConnection::input_readable_bytes() const noexcept {
    return input_buffer_.readable_bytes();
}

std::size_t TcpConnection::output_readable_bytes() const noexcept {
    return output_buffer_.readable_bytes();
}

bool TcpConnection::writing_enabled() const noexcept {
    return
        (channel_.events() & Channel::kWriteEvent) != 0U;
}

bool TcpConnection::peer_eof_received() const noexcept {
    return peer_eof_received_;
}

std::size_t TcpConnection::logger_failure_count() const noexcept {
    return logger_failure_count_;
}

void TcpConnection::handle_read() {
    if (state_ == State::Disconnected || close_notified_) {
        return;
    }

    const std::size_t readable_before = input_buffer_.readable_bytes();
    std::array<char, kReadChunkSize> bytes{};
    for (;;) {
        const ssize_t received =
            ::recv(socket_.native_handle(), bytes.data(), bytes.size(), 0);
        if (received > 0) {
            auto append_result = input_buffer_.append(
                bytes.data(),
                static_cast<std::size_t>(received));
            if (!append_result) {
                fail_connection(append_result.error());
                return;
            }
            continue;
        }
        if (received == 0) {
            peer_eof_received_ = true;
            channel_.disable_reading();
            break;
        }

        const int error_number = errno;
        if (error_number == EINTR) {
            continue;
        }
        if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
            break;
        }
        fail_connection(detail::make_system_error("recv", error_number));
        return;
    }

    if (input_buffer_.readable_bytes() > readable_before) {
        try {
            message_callback_(shared_from_this(), input_buffer_);
        } catch (const std::exception& exception) {
            safe_log(LogLevel::Error, exception.what());
            begin_close();
            return;
        } catch (...) {
            safe_log(
                LogLevel::Error,
                "message callback threw an unknown exception");
            begin_close();
            return;
        }
    }

    if (peer_eof_received_) {
        state_ = State::Disconnecting;
        shutdown_requested_ = true;
        if (output_buffer_.empty()) {
            begin_close();
        }
    }
}

void TcpConnection::handle_write() {
    if (state_ == State::Disconnected || close_notified_) {
        return;
    }

    while (!output_buffer_.empty()) {
        const std::size_t syscall_length = std::min(
            output_buffer_.readable_bytes(),
            static_cast<std::size_t>(
                std::numeric_limits<ssize_t>::max()));
        const ssize_t sent = ::send(
            socket_.native_handle(),
            output_buffer_.peek(),
            syscall_length,
            MSG_NOSIGNAL);
        if (sent > 0) {
            auto retrieve_result =
                output_buffer_.retrieve(static_cast<std::size_t>(sent));
            if (!retrieve_result) {
                fail_connection(retrieve_result.error());
                return;
            }
            rearm_high_water_if_below();
            continue;
        }
        if (sent == 0) {
            break;
        }

        const int error_number = errno;
        if (error_number == EINTR) {
            continue;
        }
        if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
            return;
        }
        fail_connection(detail::make_system_error("send", error_number));
        return;
    }

    if (!output_buffer_.empty()) {
        return;
    }
    rearm_high_water_if_below();
    channel_.disable_writing();

    if (close_after_write_requested_) {
        begin_close();
        return;
    }
    if (peer_eof_received_) {
        begin_close();
        return;
    }
    auto update_result = channel_.update();
    if (!update_result) {
        fail_connection(update_result.error());
        return;
    }
    if (shutdown_requested_) {
        auto shutdown_result = finish_write_shutdown();
        if (!shutdown_result) {
            fail_connection(shutdown_result.error());
        }
    }
}

void TcpConnection::handle_error() {
    auto error_result = socket_.socket_error();
    if (!error_result) {
        fail_connection(error_result.error());
        return;
    }
    const int error_number = error_result.value();
    if (error_number == 0) {
        fail_connection(make_error(
            ErrorCode::SystemError,
            "socket reported EPOLLERR without SO_ERROR"));
        return;
    }
    fail_connection(detail::make_system_error("socket", error_number));
}

void TcpConnection::handle_close() {
    if (state_ != State::Disconnected) {
        begin_close();
    }
}

Result<void> TcpConnection::finish_write_shutdown() {
    if (write_shutdown_) {
        return Result<void>::success();
    }
    auto result = socket_.shutdown_write();
    if (result) {
        write_shutdown_ = true;
    }
    return result;
}

void TcpConnection::begin_close() {
    if (state_ == State::Disconnected || close_notified_) {
        return;
    }
    state_ = State::Disconnecting;
    close_notified_ = true;
    if (!close_callback_) {
        state_ = State::Disconnected;
        socket_.reset();
        return;
    }
    try {
        close_callback_(shared_from_this());
    } catch (const std::exception& exception) {
        safe_log(LogLevel::Error, exception.what());
        loop_.stop();
    } catch (...) {
        safe_log(LogLevel::Error, "close callback threw an unknown exception");
        loop_.stop();
    }
}

void TcpConnection::fail_connection(const Error& error) {
    safe_log(LogLevel::Error, error.message);
    begin_close();
}

void TcpConnection::notify_high_water_if_crossed(
    const std::size_t previous_size) {
    const std::size_t current_size = output_buffer_.readable_bytes();
    if (high_water_above_ ||
        previous_size >= output_high_water_mark_ ||
        current_size < output_high_water_mark_) {
        return;
    }
    high_water_above_ = true;
    if (!high_water_callback_) {
        return;
    }
    try {
        high_water_callback_(shared_from_this(), current_size);
    } catch (const std::exception& exception) {
        safe_log(LogLevel::Error, exception.what());
    } catch (...) {
        safe_log(
            LogLevel::Error,
            "high-water callback threw an unknown exception");
    }
}

void TcpConnection::rearm_high_water_if_below() noexcept {
    if (output_buffer_.readable_bytes() < output_high_water_mark_) {
        high_water_above_ = false;
    }
}

void TcpConnection::safe_log(
    const LogLevel level,
    const std::string_view message) noexcept {
    try {
        logger_.log(level, "TcpConnection", message);
    } catch (...) {
        ++logger_failure_count_;
    }
}

}  // namespace iaisf::net::tcp
