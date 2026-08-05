#include "iaisf/http/http_session.hpp"

#include "iaisf/metrics/metrics.hpp"

#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace iaisf::http {

namespace {

template <typename Metric, typename Create, typename Get>
std::shared_ptr<Metric> metric_or_existing(
    Create&& create,
    Get&& get) noexcept {
    try {
        auto created = create();
        if (created) {
            return created.value();
        }
        auto existing = get();
        if (existing) {
            return existing.value();
        }
    } catch (...) {
        // Metrics are observational and never affect protocol behavior.
    }
    return {};
}

}  // namespace

Result<HttpSession::Ptr> HttpSession::create(
    net::EventLoop& loop,
    const HttpRouter& router,
    HttpLimits limits,
    const ConnectionPtr& connection,
    std::optional<std::chrono::steady_clock::duration> header_timeout,
    std::optional<std::chrono::steady_clock::duration> body_timeout,
    MetricsRegistry* const metrics) {
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
    try {
        auto session = std::shared_ptr<HttpSession>{
            new HttpSession{
                loop,
                router,
                std::move(limits),
                connection,
                header_timeout,
                body_timeout,
                metrics}};
        session->initialize_metrics();
        return Result<Ptr>::success(std::move(session));
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
    const ConnectionPtr& connection,
    std::optional<std::chrono::steady_clock::duration> header_timeout,
    std::optional<std::chrono::steady_clock::duration> body_timeout,
    MetricsRegistry* const metrics)
    : loop_(loop),
      router_(router),
      limits_(std::move(limits)),
      parser_(limits_),
      connection_(connection),
      header_timeout_(header_timeout),
      body_timeout_(body_timeout),
      metrics_(metrics) {}

void HttpSession::initialize_metrics() noexcept {
    if (metrics_ == nullptr) {
        return;
    }
    requests_metric_ = metric_or_existing<Counter>(
        [this] { return metrics_->create_counter("http_requests_total"); },
        [this] { return metrics_->get_counter("http_requests_total"); });
    responses_metric_ = metric_or_existing<Counter>(
        [this] { return metrics_->create_counter("http_responses_total"); },
        [this] { return metrics_->get_counter("http_responses_total"); });
    timeout_408_metric_ = metric_or_existing<Counter>(
        [this] { return metrics_->create_counter("http_408_timeout_total"); },
        [this] { return metrics_->get_counter("http_408_timeout_total"); });
    parse_errors_metric_ = metric_or_existing<Counter>(
        [this] { return metrics_->create_counter("http_parse_errors_total"); },
        [this] { return metrics_->get_counter("http_parse_errors_total"); });
}

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
    cancel_header_timeout();
    cancel_body_timeout();
    terminal_ = true;
    receive_state_ = ReceiveState::Terminal;
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
        if (receive_state_ == ReceiveState::AwaitingRequest) {
            auto timer_result = begin_header_timeout();
            if (!timer_result) {
                input.retrieve_all();
                close_without_response(connection);
                return;
            }
        }
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
        if (receive_state_ == ReceiveState::ReadingHeaders &&
            progress.phase != ParsePhase::Headers) {
            cancel_header_timeout();
            if (progress.phase == ParsePhase::Body) {
                receive_state_ = ReceiveState::ReadingBody;
                auto timer_result = begin_body_timeout();
                if (!timer_result) {
                    input.retrieve_all();
                    close_without_response(connection);
                    return;
                }
            } else {
                receive_state_ = ReceiveState::AfterHeaders;
            }
        } else if (receive_state_ == ReceiveState::ReadingBody) {
            if (progress.phase != ParsePhase::Body) {
                cancel_body_timeout();
                receive_state_ = ReceiveState::AfterHeaders;
            } else if (progress.body_bytes_consumed > 0U) {
                auto timer_result = refresh_body_timeout();
                if (!timer_result) {
                    input.retrieve_all();
                    close_without_response(connection);
                    return;
                }
            }
        }
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
            if (parse_errors_metric_) {
                parse_errors_metric_->increment();
            }
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
        if (requests_metric_) {
            requests_metric_->increment();
        }
        receive_state_ = ReceiveState::AwaitingRequest;
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
        if (receive_state_ == ReceiveState::AwaitingRequest) {
            auto timer_result = begin_header_timeout();
            if (!timer_result) {
                input.retrieve_all();
                close_without_response(connection);
                return;
            }
        }
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
    if (responses_metric_) {
        responses_metric_->increment();
    }
    if (close_after_response) {
        cancel_header_timeout();
        cancel_body_timeout();
        receive_state_ = ReceiveState::Terminal;
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
    cancel_header_timeout();
    cancel_body_timeout();
    receive_state_ = ReceiveState::Terminal;
    HttpResponse response;
    try {
        response = HttpResponse::error(status, true);
        if (status == HttpStatus::RequestTimeout && timeout_408_metric_) {
            timeout_408_metric_->increment();
        }
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
    if (responses_metric_) {
        responses_metric_->increment();
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
    cancel_header_timeout();
    cancel_body_timeout();
    terminal_ = true;
    receive_state_ = ReceiveState::Terminal;
    continuation_pending_ = false;
    continuation_input_ = nullptr;
    if (connection) {
        const auto close_result = connection->force_close();
        if (!close_result) {
            terminal_ = true;
        }
    }
}

Result<void> HttpSession::begin_header_timeout() {
    if (receive_state_ != ReceiveState::AwaitingRequest ||
        header_timer_.has_value()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP header timeout cannot start in the current state"));
    }
    if (!header_timeout_.has_value()) {
        receive_state_ = ReceiveState::ReadingHeaders;
        return Result<void>::success();
    }
    if (header_generation_ ==
        std::numeric_limits<std::uint64_t>::max()) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP header timeout generation is exhausted"));
    }

    ++header_generation_;
    const std::uint64_t generation = header_generation_;
    try {
        const std::weak_ptr<HttpSession> weak_session = weak_from_this();
        auto timer_result = loop_.run_after(
            *header_timeout_,
            [weak_session, generation] {
                if (const auto session = weak_session.lock()) {
                    session->handle_header_timeout(generation);
                }
            });
        if (!timer_result) {
            return Result<void>::failure(std::move(timer_result).error());
        }
        header_timer_ = std::move(timer_result).value();
        receive_state_ = ReceiveState::ReadingHeaders;
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP header timeout callback allocation failed"));
    } catch (...) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "HTTP header timeout scheduling failed"));
    }
}

void HttpSession::cancel_header_timeout() noexcept {
    if (!header_timer_.has_value()) {
        return;
    }
    const net::TimerId timer = *header_timer_;
    header_timer_.reset();
    if (header_generation_ !=
        std::numeric_limits<std::uint64_t>::max()) {
        ++header_generation_;
    }
    try {
        auto cancel_result = loop_.cancel_timer(timer);
        static_cast<void>(cancel_result);
    } catch (...) {
        // Generation invalidation is the safety boundary. Cancellation is a
        // best-effort resource optimization during protocol state changes.
    }
}

void HttpSession::handle_header_timeout(
    const std::uint64_t generation) noexcept {
    if (!header_timer_.has_value() ||
        generation != header_generation_ || terminal_ ||
        receive_state_ != ReceiveState::ReadingHeaders) {
        return;
    }
    header_timer_.reset();
    if (header_generation_ !=
        std::numeric_limits<std::uint64_t>::max()) {
        ++header_generation_;
    }
    continuation_pending_ = false;
    continuation_input_ = nullptr;

    const auto connection = connection_.lock();
    if (!connection) {
        terminal_ = true;
        receive_state_ = ReceiveState::Terminal;
        return;
    }
    send_error(connection, HttpStatus::RequestTimeout);
}

Result<void> HttpSession::begin_body_timeout() {
    if (receive_state_ != ReceiveState::ReadingBody ||
        body_timer_.has_value()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP body timeout cannot start in the current state"));
    }
    if (!body_timeout_.has_value()) {
        return Result<void>::success();
    }
    if (body_generation_ ==
        std::numeric_limits<std::uint64_t>::max()) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP body timeout generation is exhausted"));
    }

    ++body_generation_;
    const std::uint64_t generation = body_generation_;
    try {
        const std::weak_ptr<HttpSession> weak_session = weak_from_this();
        auto timer_result = loop_.run_after(
            *body_timeout_,
            [weak_session, generation] {
                if (const auto session = weak_session.lock()) {
                    session->handle_body_timeout(generation);
                }
            });
        if (!timer_result) {
            return Result<void>::failure(std::move(timer_result).error());
        }
        body_timer_ = std::move(timer_result).value();
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "HTTP body timeout callback allocation failed"));
    } catch (...) {
        return Result<void>::failure(make_error(
            ErrorCode::InternalError,
            "HTTP body timeout scheduling failed"));
    }
}

Result<void> HttpSession::refresh_body_timeout() {
    if (receive_state_ != ReceiveState::ReadingBody) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "HTTP body timeout cannot refresh in the current state"));
    }
    cancel_body_timeout();
    return begin_body_timeout();
}

void HttpSession::cancel_body_timeout() noexcept {
    if (!body_timer_.has_value()) {
        return;
    }
    const net::TimerId timer = *body_timer_;
    body_timer_.reset();
    if (body_generation_ !=
        std::numeric_limits<std::uint64_t>::max()) {
        ++body_generation_;
    }
    try {
        auto cancel_result = loop_.cancel_timer(timer);
        static_cast<void>(cancel_result);
    } catch (...) {
        // Generation invalidation is the safety boundary. Cancellation is a
        // best-effort resource optimization during protocol state changes.
    }
}

void HttpSession::handle_body_timeout(
    const std::uint64_t generation) noexcept {
    if (!body_timer_.has_value() || generation != body_generation_ ||
        terminal_ || receive_state_ != ReceiveState::ReadingBody) {
        return;
    }
    body_timer_.reset();
    if (body_generation_ !=
        std::numeric_limits<std::uint64_t>::max()) {
        ++body_generation_;
    }
    continuation_pending_ = false;
    continuation_input_ = nullptr;

    const auto connection = connection_.lock();
    if (!connection) {
        terminal_ = true;
        receive_state_ = ReceiveState::Terminal;
        return;
    }
    send_error(connection, HttpStatus::RequestTimeout);
}

}  // namespace iaisf::http
