#include "iaisf/net/tcp/tcp_server.hpp"

#include <exception>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::net::tcp {

static_assert(
    TcpServerOptions::kMaximumBufferBytes ==
    static_cast<std::int64_t>(TcpConnection::kMaximumBufferBytes));

TcpServerOptions::TcpServerOptions(
    const int listen_backlog,
    const std::size_t max_connections,
    const std::size_t input_initial_capacity,
    const std::size_t input_maximum_capacity,
    const std::size_t output_initial_capacity,
    const std::size_t output_high_water_mark,
    const std::size_t output_maximum_capacity,
    std::optional<int> socket_send_buffer_bytes) noexcept
    : listen_backlog_(listen_backlog),
      max_connections_(max_connections),
      input_initial_capacity_(input_initial_capacity),
      input_maximum_capacity_(input_maximum_capacity),
      output_initial_capacity_(output_initial_capacity),
      output_high_water_mark_(output_high_water_mark),
      output_maximum_capacity_(output_maximum_capacity),
      socket_send_buffer_bytes_(socket_send_buffer_bytes) {}

Result<TcpServerOptions> TcpServerOptions::create(
    const std::int64_t listen_backlog,
    const std::int64_t max_connections,
    const std::int64_t input_initial_capacity,
    const std::int64_t input_maximum_capacity,
    const std::int64_t output_initial_capacity,
    const std::int64_t output_high_water_mark,
    const std::int64_t output_maximum_capacity,
    const std::optional<std::int64_t> socket_send_buffer_bytes) {
    if (listen_backlog <= 0 ||
        listen_backlog > Acceptor::kMaximumBacklog) {
        return Result<TcpServerOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TCP listen backlog is outside the supported range"));
    }
    if (max_connections <= 0 ||
        max_connections > kMaximumConnections) {
        return Result<TcpServerOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TCP max_connections is outside the supported range"));
    }
    if (input_initial_capacity <= 0 ||
        input_maximum_capacity <= 0 ||
        input_initial_capacity > input_maximum_capacity ||
        input_maximum_capacity > kMaximumBufferBytes) {
        return Result<TcpServerOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TCP input buffer capacities are inconsistent"));
    }
    if (output_initial_capacity <= 0 ||
        output_high_water_mark <= 0 ||
        output_maximum_capacity <= 0 ||
        output_initial_capacity > output_maximum_capacity ||
        output_high_water_mark > output_maximum_capacity ||
        output_maximum_capacity > kMaximumBufferBytes) {
        return Result<TcpServerOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TCP output buffer capacities are inconsistent"));
    }
    if (socket_send_buffer_bytes.has_value() &&
        (*socket_send_buffer_bytes <= 0 ||
         *socket_send_buffer_bytes > kMaximumBufferBytes)) {
        return Result<TcpServerOptions>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TCP socket send buffer size is outside the supported range"));
    }

    return Result<TcpServerOptions>::success(TcpServerOptions{
        static_cast<int>(listen_backlog),
        static_cast<std::size_t>(max_connections),
        static_cast<std::size_t>(input_initial_capacity),
        static_cast<std::size_t>(input_maximum_capacity),
        static_cast<std::size_t>(output_initial_capacity),
        static_cast<std::size_t>(output_high_water_mark),
        static_cast<std::size_t>(output_maximum_capacity),
        socket_send_buffer_bytes.has_value()
            ? std::optional<int>{
                  static_cast<int>(*socket_send_buffer_bytes)}
            : std::nullopt});
}

TcpServerOptions TcpServerOptions::defaults() noexcept {
    return TcpServerOptions{
        128,
        1024U,
        4096U,
        1024U * 1024U,
        4096U,
        64U * 1024U,
        1024U * 1024U,
        std::nullopt};
}

int TcpServerOptions::listen_backlog() const noexcept {
    return listen_backlog_;
}

std::size_t TcpServerOptions::max_connections() const noexcept {
    return max_connections_;
}

std::size_t TcpServerOptions::input_initial_capacity() const noexcept {
    return input_initial_capacity_;
}

std::size_t TcpServerOptions::input_maximum_capacity() const noexcept {
    return input_maximum_capacity_;
}

std::size_t TcpServerOptions::output_initial_capacity() const noexcept {
    return output_initial_capacity_;
}

std::size_t TcpServerOptions::output_high_water_mark() const noexcept {
    return output_high_water_mark_;
}

std::size_t TcpServerOptions::output_maximum_capacity() const noexcept {
    return output_maximum_capacity_;
}

std::optional<int> TcpServerOptions::socket_send_buffer_bytes() const noexcept {
    return socket_send_buffer_bytes_;
}

Result<TcpServer::Ptr> TcpServer::create(
    EventLoop& loop,
    ILogger& logger,
    const Ipv4Endpoint& bind_endpoint,
    TcpServerOptions options) {
    if (!loop.is_in_loop_thread()) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpServer must be created in the EventLoop owner thread"));
    }
    auto acceptor_result = Acceptor::create(
        loop,
        logger,
        bind_endpoint,
        options.listen_backlog());
    if (!acceptor_result) {
        return Result<Ptr>::failure(acceptor_result.error());
    }

    try {
        Ptr server{new TcpServer{
            loop,
            logger,
            std::move(options),
            std::move(acceptor_result).value()}};
        server->pending_removals_.reserve(
            server->options_.max_connections());
        server->stop_snapshot_.reserve(
            server->options_.max_connections());
        return Result<Ptr>::success(std::move(server));
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "TcpServer allocation failed"));
    }
}

TcpServer::TcpServer(
    EventLoop& loop,
    ILogger& logger,
    TcpServerOptions options,
    std::unique_ptr<Acceptor> acceptor) noexcept
    : loop_(loop),
      logger_(logger),
      options_(std::move(options)),
      acceptor_(std::move(acceptor)),
      deferred_cleanup_(this, &TcpServer::run_deferred_cleanup) {}

TcpServer::~TcpServer() noexcept {
    if (!loop_.is_in_loop_thread()) {
        std::terminate();
    }
    if (state_ == State::Created) {
        auto stop_result = acceptor_->stop();
        if (!stop_result) {
            std::terminate();
        }
        state_ = State::Stopped;
    }
    if (state_ != State::Stopped ||
        !acceptor_->stopped() ||
        !connections_.empty() ||
        !pending_removals_.empty() ||
        !stop_snapshot_.empty() ||
        deferred_cleanup_.pending()) {
        std::terminate();
    }
}

Result<void> TcpServer::start(
    MessageCallback message_callback,
    ConnectionCallback connection_callback,
    HighWaterCallback high_water_callback) {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpServer::start must run in the EventLoop owner thread"));
    }
    if (state_ != State::Created) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpServer can only be started once"));
    }
    if (!message_callback) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "TcpServer message callback must not be empty"));
    }

    try {
        message_callback_ = std::move(message_callback);
        connection_callback_ = std::move(connection_callback);
        high_water_callback_ = std::move(high_water_callback);
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "TcpServer callback allocation failed"));
    }

    const std::weak_ptr<TcpServer> weak_self = weak_from_this();
    auto start_result = acceptor_->start(
        [weak_self](Socket socket, const Ipv4Endpoint& peer) {
            if (const auto self = weak_self.lock()) {
                self->handle_new_connection(std::move(socket), peer);
            }
        });
    if (!start_result) {
        message_callback_ = {};
        connection_callback_ = {};
        high_water_callback_ = {};
        return start_result;
    }
    state_ = State::Running;
    return Result<void>::success();
}

Result<void> TcpServer::stop() {
    if (!loop_.is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "TcpServer::stop must run in the EventLoop owner thread"));
    }
    if (state_ == State::Stopping || state_ == State::Stopped) {
        return Result<void>::success();
    }
    state_ = State::Stopping;

    auto acceptor_result = acceptor_->stop();
    if (!acceptor_result) {
        return acceptor_result;
    }

    Error first_error;
    bool has_error = false;
    stop_snapshot_.clear();
    for (const auto& entry : connections_) {
        if (stop_snapshot_.size() >= stop_snapshot_.capacity()) {
            std::terminate();
        }
        stop_snapshot_.push_back(entry.second);
    }
    for (const auto& connection : stop_snapshot_) {
        auto close_result = connection->force_close();
        if (!close_result && !has_error) {
            first_error = close_result.error();
            has_error = true;
        }
    }
    stop_snapshot_.clear();

    if (loop_.dispatching_active_channels()) {
        auto defer_result = loop_.defer_cleanup(deferred_cleanup_);
        if (!defer_result && !has_error) {
            first_error = defer_result.error();
            has_error = true;
        }
    } else {
        drain_pending_removals();
    }
    complete_stop_if_ready();

    if (has_error) {
        return Result<void>::failure(std::move(first_error));
    }
    return Result<void>::success();
}

bool TcpServer::started() const noexcept {
    return state_ == State::Running;
}

bool TcpServer::stopped() const noexcept {
    return state_ == State::Stopped;
}

const Ipv4Endpoint& TcpServer::local_endpoint() const noexcept {
    return acceptor_->local_endpoint();
}

std::size_t TcpServer::connection_count() const noexcept {
    return connections_.size();
}

std::size_t TcpServer::rejected_connection_count() const noexcept {
    return rejected_connection_count_;
}

std::size_t TcpServer::logger_failure_count() const noexcept {
    return logger_failure_count_;
}

void TcpServer::handle_new_connection(
    Socket socket,
    const Ipv4Endpoint& peer) {
    if (state_ != State::Running) {
        ++rejected_connection_count_;
        return;
    }
    if (connections_.size() >= options_.max_connections()) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Warn, "maximum TCP connection count reached");
        return;
    }
    if (next_connection_id_ ==
        std::numeric_limits<std::uint64_t>::max()) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, "TCP connection id space exhausted");
        return;
    }

    auto no_delay_result = socket.set_no_delay();
    if (!no_delay_result) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, no_delay_result.error().message);
        return;
    }
    const auto send_buffer_bytes =
        options_.socket_send_buffer_bytes();
    if (send_buffer_bytes.has_value()) {
        auto send_buffer_result = socket.set_send_buffer_size(
            *send_buffer_bytes);
        if (!send_buffer_result) {
            ++rejected_connection_count_;
            safe_log(LogLevel::Error, send_buffer_result.error().message);
            return;
        }
    }
    auto local_result = socket.local_endpoint();
    if (!local_result) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, local_result.error().message);
        return;
    }

    const std::uint64_t connection_id = next_connection_id_;
    auto connection_result = TcpConnection::create(
        loop_,
        logger_,
        connection_id,
        std::move(socket),
        std::move(local_result).value(),
        peer,
        options_.input_initial_capacity(),
        options_.input_maximum_capacity(),
        options_.output_initial_capacity(),
        options_.output_maximum_capacity(),
        options_.output_high_water_mark());
    if (!connection_result) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, connection_result.error().message);
        return;
    }
    ConnectionPtr connection = std::move(connection_result).value();

    auto callback_result =
        connection->set_message_callback(message_callback_);
    if (!callback_result) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, callback_result.error().message);
        return;
    }
    callback_result =
        connection->set_connection_callback(connection_callback_);
    if (!callback_result) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, callback_result.error().message);
        return;
    }
    callback_result =
        connection->set_high_water_callback(high_water_callback_);
    if (!callback_result) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, callback_result.error().message);
        return;
    }

    const std::weak_ptr<TcpServer> weak_self = weak_from_this();
    callback_result = connection->set_close_callback(
        [weak_self](const ConnectionPtr& closing) noexcept {
            if (const auto self = weak_self.lock()) {
                self->schedule_remove_connection(closing);
            }
        });
    if (!callback_result) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, callback_result.error().message);
        return;
    }

    try {
        const auto insertion =
            connections_.emplace(connection_id, connection);
        if (!insertion.second) {
            ++rejected_connection_count_;
            safe_log(LogLevel::Error, "duplicate TCP connection id");
            return;
        }
    } catch (const std::bad_alloc&) {
        ++rejected_connection_count_;
        safe_log(LogLevel::Error, "TCP connection table allocation failed");
        return;
    }
    ++next_connection_id_;

    auto establish_result = connection->connect_established();
    if (!establish_result) {
        safe_log(LogLevel::Error, establish_result.error().message);
        schedule_remove_connection(connection);
    }
}

void TcpServer::schedule_remove_connection(
    const ConnectionPtr& connection) noexcept {
    if (!loop_.is_in_loop_thread()) {
        std::terminate();
    }
    const auto iterator = connections_.find(connection->id());
    if (iterator == connections_.end() ||
        iterator->second.get() != connection.get()) {
        return;
    }
    if (pending_removals_.size() >= pending_removals_.capacity()) {
        std::terminate();
    }
    pending_removals_.push_back(connection);

    if (draining_removals_) {
        return;
    }
    if (!loop_.dispatching_active_channels()) {
        drain_pending_removals();
        return;
    }

    auto defer_result = loop_.defer_cleanup(deferred_cleanup_);
    if (!defer_result) {
        safe_log(LogLevel::Error, defer_result.error().message);
        loop_.stop();
    }
}

void TcpServer::remove_connection(
    const ConnectionPtr& connection) noexcept {
    const auto iterator = connections_.find(connection->id());
    if (iterator == connections_.end() ||
        iterator->second.get() != connection.get()) {
        return;
    }

    auto destroy_result = connection->connect_destroyed();
    if (!destroy_result) {
        safe_log(LogLevel::Error, destroy_result.error().message);
        loop_.stop();
        return;
    }
    connections_.erase(iterator);
}

void TcpServer::drain_pending_removals() noexcept {
    if (draining_removals_) {
        return;
    }
    draining_removals_ = true;
    while (!pending_removals_.empty()) {
        ConnectionPtr connection =
            std::move(pending_removals_.back());
        pending_removals_.pop_back();
        remove_connection(connection);
    }
    draining_removals_ = false;
    complete_stop_if_ready();
}

void TcpServer::complete_stop_if_ready() noexcept {
    if (state_ == State::Stopping &&
        acceptor_->stopped() &&
        connections_.empty() &&
        pending_removals_.empty() &&
        !draining_removals_) {
        state_ = State::Stopped;
        connection_callback_ = {};
        message_callback_ = {};
        high_water_callback_ = {};
        return;
    }
    if (state_ == State::Stopping && !acceptor_->stopped()) {
        auto defer_result = loop_.defer_cleanup(deferred_cleanup_);
        if (!defer_result) {
            safe_log(LogLevel::Error, defer_result.error().message);
            loop_.stop();
        }
    }
}

void TcpServer::run_deferred_cleanup(void* const context) noexcept {
    static_cast<TcpServer*>(context)->drain_pending_removals();
}

void TcpServer::safe_log(
    const LogLevel level,
    const std::string_view message) noexcept {
    try {
        logger_.log(level, "TcpServer", message);
    } catch (...) {
        ++logger_failure_count_;
    }
}

}  // namespace iaisf::net::tcp
