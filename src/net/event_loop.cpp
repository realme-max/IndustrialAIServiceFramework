#include "iaisf/net/event_loop.hpp"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

#include "iaisf/net/channel.hpp"
#include "iaisf/net/epoll_poller.hpp"
#include "system_error.hpp"

namespace iaisf::net {

Result<std::unique_ptr<EventLoop>> EventLoop::create(
    ILogger& logger,
    const std::size_t max_events,
    const std::size_t pending_callback_capacity) {
    if (pending_callback_capacity == 0U ||
        pending_callback_capacity > kMaximumPendingCallbacks) {
        return Result<std::unique_ptr<EventLoop>>::failure(make_error(
            ErrorCode::InvalidArgument,
            "pending callback capacity is out of range"));
    }

    auto poller_result = EpollPoller::create(max_events);
    if (!poller_result) {
        return Result<std::unique_ptr<EventLoop>>::failure(
            std::move(poller_result).error());
    }

    const int wakeup_fd = ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd < 0) {
        const int error_number = errno;
        return Result<std::unique_ptr<EventLoop>>::failure(
            detail::make_system_error("eventfd", error_number));
    }

    auto loop = std::unique_ptr<EventLoop>{new EventLoop{
        logger,
        std::move(poller_result).value(),
        UniqueFd{wakeup_fd},
        pending_callback_capacity}};
    auto initialization_result = loop->initialize_wakeup_channel();
    if (!initialization_result) {
        return Result<std::unique_ptr<EventLoop>>::failure(
            std::move(initialization_result).error());
    }
    return Result<std::unique_ptr<EventLoop>>::success(std::move(loop));
}

EventLoop::EventLoop(
    ILogger& logger,
    std::unique_ptr<EpollPoller> poller,
    UniqueFd wakeup_fd,
    const std::size_t pending_callback_capacity)
    : logger_(logger),
      owner_thread_(std::this_thread::get_id()),
      poller_(std::move(poller)),
      wakeup_fd_(std::move(wakeup_fd)),
      pending_callback_capacity_(pending_callback_capacity) {}

EventLoop::~EventLoop() noexcept {
    const State current = state_.load(std::memory_order_acquire);
    if (!is_in_loop_thread() ||
        current == State::Running ||
        current == State::Stopping) {
        std::terminate();
    }

    stop();
    if (wakeup_channel_ && wakeup_channel_->is_registered()) {
        auto remove_result = poller_->remove(*wakeup_channel_);
        if (!remove_result) {
            safe_log(LogLevel::Error, "failed to remove EventLoop wakeup channel");
        }
    }
    wakeup_channel_.reset();
}

Result<void> EventLoop::initialize_wakeup_channel() {
    wakeup_channel_ = std::make_unique<Channel>(*this, wakeup_fd_.get());
    wakeup_channel_->set_read_callback([this] { drain_wakeup(); });
    wakeup_channel_->enable_reading();
    wakeup_channel_->set_edge_triggered(true);
    return update_channel(*wakeup_channel_);
}

Result<void> EventLoop::run() {
    if (!is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "EventLoop::run must execute on the owner thread"));
    }

    {
        std::lock_guard<std::mutex> lock{pending_mutex_};
        if (state_.load(std::memory_order_acquire) != State::Created) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidState,
                "EventLoop cannot be run from its current state"));
        }
        state_.store(State::Running, std::memory_order_release);
    }

    std::vector<Channel*> active_channels;
    active_channels.reserve(poller_->max_events());

    while (state_.load(std::memory_order_acquire) == State::Running) {
        auto poll_result = poller_->poll(-1, active_channels);
        if (!poll_result) {
            safe_log(LogLevel::Error, "epoll_wait failed; EventLoop is stopping");
            transition_to_stopped();
            return Result<void>::failure(std::move(poll_result).error());
        }

        handle_active_channels(active_channels);
        execute_pending_callbacks();
    }

    drain_wakeup();
    execute_pending_callbacks();
    transition_to_stopped();
    return Result<void>::success();
}

void EventLoop::stop() noexcept {
    bool should_wake = false;
    std::deque<Callback> cancelled_callbacks;
    {
        std::lock_guard<std::mutex> lock{pending_mutex_};
        const State current = state_.load(std::memory_order_acquire);
        if (current == State::Created) {
            state_.store(State::Stopped, std::memory_order_release);
            cancelled_callbacks.swap(pending_callbacks_);
        } else if (current == State::Running) {
            state_.store(State::Stopping, std::memory_order_release);
            should_wake = true;
        }
    }

    if (should_wake) {
        auto wakeup_result = signal_wakeup();
        if (!wakeup_result) {
            safe_log(LogLevel::Error, "failed to signal EventLoop stop");
        }
    }
}

bool EventLoop::is_in_loop_thread() const noexcept {
    return owner_thread_ == std::this_thread::get_id();
}

EventLoop::State EventLoop::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

Result<void> EventLoop::update_channel(Channel& channel) {
    if (!is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "channel updates require the EventLoop owner thread"));
    }
    if (!channel.belongs_to(*this)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "channel belongs to a different EventLoop"));
    }

    const State current = state_.load(std::memory_order_acquire);
    if ((current == State::Stopping || current == State::Stopped) &&
        channel.has_events()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "stopped EventLoop cannot register or update channel events"));
    }

    if (!channel.has_events()) {
        if (!channel.is_registered()) {
            return Result<void>::success();
        }
        if (dispatching_active_channels_) {
            return Result<void>::failure(make_error(
                ErrorCode::InvalidState,
                "channel removal must be deferred until the active batch completes"));
        }
        return poller_->remove(channel);
    }
    if (channel.is_registered()) {
        return poller_->update(channel);
    }
    return poller_->add(channel);
}

Result<void> EventLoop::remove_channel(Channel& channel) {
    if (!is_in_loop_thread()) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "channel removal requires the EventLoop owner thread"));
    }
    if (!channel.belongs_to(*this)) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "channel belongs to a different EventLoop"));
    }
    if (dispatching_active_channels_) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "channel removal must be deferred until the active batch completes"));
    }
    return poller_->remove(channel);
}

Result<void> EventLoop::queue_in_loop(Callback callback) {
    if (!callback) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "queued callback must not be empty"));
    }

    std::lock_guard<std::mutex> lock{pending_mutex_};
    const State current = state_.load(std::memory_order_acquire);
    if (current == State::Stopping || current == State::Stopped) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "EventLoop is not accepting callbacks"));
    }
    if (pending_callbacks_.size() >= pending_callback_capacity_) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "pending callback queue is full"));
    }

    try {
        pending_callbacks_.push_back(std::move(callback));
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "pending callback allocation failed"));
    }
    // The queue mutex is intentionally held through the nonblocking eventfd
    // write. This makes acceptance atomic with rollback: a returned failure
    // cannot leave this callback visible to the EventLoop.
    auto wakeup_result = signal_wakeup();
    if (!wakeup_result) {
        pending_callbacks_.pop_back();
        return wakeup_result;
    }
    return Result<void>::success();
}

std::size_t EventLoop::pending_callback_count() const {
    std::lock_guard<std::mutex> lock{pending_mutex_};
    return pending_callbacks_.size();
}

std::size_t EventLoop::pending_callback_capacity() const noexcept {
    return pending_callback_capacity_;
}

std::size_t EventLoop::logger_failure_count() const noexcept {
    return logger_failure_count_.load(std::memory_order_relaxed);
}

Result<void> EventLoop::signal_wakeup() {
    constexpr std::uint64_t signal_value = 1U;
    while (true) {
        const ssize_t written =
            ::write(wakeup_fd_.get(), &signal_value, sizeof(signal_value));
        if (written == static_cast<ssize_t>(sizeof(signal_value))) {
            return Result<void>::success();
        }
        if (written < 0) {
            const int error_number = errno;
            if (error_number == EINTR) {
                continue;
            }
            if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
                return Result<void>::success();
            }
            return Result<void>::failure(
                detail::make_system_error("eventfd write", error_number));
        }
        return Result<void>::failure(make_error(
            ErrorCode::SystemError,
            "eventfd write produced an incomplete result"));
    }
}

void EventLoop::drain_wakeup() noexcept {
    std::uint64_t value = 0U;
    while (true) {
        const ssize_t bytes_read = ::read(wakeup_fd_.get(), &value, sizeof(value));
        if (bytes_read == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (bytes_read < 0) {
            const int error_number = errno;
            if (error_number == EINTR) {
                continue;
            }
            if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
                return;
            }
            safe_log(LogLevel::Error, "failed to drain EventLoop wakeup fd");
            return;
        }
        safe_log(LogLevel::Error, "EventLoop wakeup fd returned an incomplete read");
        return;
    }
}

void EventLoop::execute_pending_callbacks() noexcept {
    std::deque<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock{pending_mutex_};
        callbacks.swap(pending_callbacks_);
    }

    for (auto& callback : callbacks) {
        try {
            callback();
        } catch (const std::exception&) {
            safe_log(LogLevel::Error, "EventLoop callback threw std::exception");
        } catch (...) {
            safe_log(LogLevel::Error, "EventLoop callback threw an unknown exception");
        }
    }
}

void EventLoop::handle_active_channels(
    const std::vector<Channel*>& active_channels) noexcept {
    assert(!dispatching_active_channels_);
    dispatching_active_channels_ = true;
    for (Channel* const channel : active_channels) {
        if (channel == nullptr) {
            safe_log(LogLevel::Error, "EventLoop received a null Channel");
            continue;
        }
        try {
            channel->handle_event();
        } catch (const std::exception&) {
            safe_log(LogLevel::Error, "Channel callback threw std::exception");
        } catch (...) {
            safe_log(LogLevel::Error, "Channel callback threw an unknown exception");
        }
    }
    dispatching_active_channels_ = false;
}

void EventLoop::transition_to_stopped() noexcept {
    std::lock_guard<std::mutex> lock{pending_mutex_};
    state_.store(State::Stopped, std::memory_order_release);
}

void EventLoop::safe_log(const LogLevel level, const char* const message) noexcept {
    try {
        logger_.log(level, "EventLoop", message);
    } catch (...) {
        logger_failure_count_.fetch_add(1U, std::memory_order_relaxed);
    }
}

}  // namespace iaisf::net
