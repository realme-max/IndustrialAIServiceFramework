#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net {

class Channel;
class EpollPoller;

/**
 * Single-thread Linux Reactor core with bounded cross-thread callback input.
 *
 * run(), update_channel(), and remove_channel() are owner-thread-only.
 * stop() and queue_in_loop() are thread-safe. The injected logger is not owned
 * and must outlive the EventLoop. Destruction is owner-thread-only and is
 * forbidden while the loop is Running or Stopping.
 *
 * Channel removal is rejected while an active event batch is being dispatched.
 * A Channel callback must use queue_in_loop() to defer removal and destruction
 * until the complete active batch has finished.
 */
class EventLoop final {
public:
    using Callback = std::function<void()>;

    enum class State {
        Created,
        Running,
        Stopping,
        Stopped,
    };

    static constexpr std::size_t kMaximumPendingCallbacks = 1'000'000U;

    [[nodiscard]] static Result<std::unique_ptr<EventLoop>> create(
        ILogger& logger,
        std::size_t max_events = 256U,
        std::size_t pending_callback_capacity = 1024U);

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;
    ~EventLoop() noexcept;

    [[nodiscard]] Result<void> run();
    void stop() noexcept;

    [[nodiscard]] bool is_in_loop_thread() const noexcept;
    [[nodiscard]] State state() const noexcept;

    [[nodiscard]] Result<void> update_channel(Channel& channel);
    [[nodiscard]] Result<void> remove_channel(Channel& channel);
    [[nodiscard]] Result<void> queue_in_loop(Callback callback);

    [[nodiscard]] std::size_t pending_callback_count() const;
    [[nodiscard]] std::size_t pending_callback_capacity() const noexcept;
    [[nodiscard]] std::size_t logger_failure_count() const noexcept;

private:
    friend struct EventLoopTestAccess;

    EventLoop(
        ILogger& logger,
        std::unique_ptr<EpollPoller> poller,
        UniqueFd wakeup_fd,
        std::size_t pending_callback_capacity);

    [[nodiscard]] Result<void> initialize_wakeup_channel();
    [[nodiscard]] Result<void> signal_wakeup();
    void drain_wakeup() noexcept;
    void execute_pending_callbacks() noexcept;
    void handle_active_channels(const std::vector<Channel*>& active_channels) noexcept;
    void transition_to_stopped() noexcept;
    void safe_log(LogLevel level, const char* message) noexcept;

    ILogger& logger_;
    const std::thread::id owner_thread_;
    std::unique_ptr<EpollPoller> poller_;
    UniqueFd wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;
    const std::size_t pending_callback_capacity_;

    mutable std::mutex pending_mutex_;
    std::deque<Callback> pending_callbacks_;
    std::atomic<State> state_{State::Created};
    std::atomic<std::size_t> logger_failure_count_{0U};
    bool dispatching_active_channels_{false};
};

}  // namespace iaisf::net
