#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "iaisf/core/result.hpp"
#include "iaisf/logging/logger.hpp"
#include "iaisf/net/timer.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net {

class Channel;
class EpollPoller;

namespace detail {
class TimerQueue;
class TimerQueueTestAccess;
}  // namespace detail

/**
 * Single-thread Linux Reactor core with bounded cross-thread callback input.
 *
 * run(), update_channel(), and remove_channel() are owner-thread-only.
 * stop() and queue_in_loop() are thread-safe. The injected logger is not owned
 * and must outlive the EventLoop. Destruction is owner-thread-only and is
 * forbidden while the loop is Running or Stopping.
 *
 * Channel removal is rejected while an active event batch is being dispatched.
 * Application work uses queue_in_loop(). Framework-owned lifecycle objects use
 * an embedded DeferredCleanup node so cleanup cannot be rejected by the
 * application queue capacity and cannot allocate while being scheduled.
 */
class EventLoop final {
public:
    using Callback = std::function<void()>;

    /**
     * Intrusive, allocation-free node for framework lifecycle cleanup.
     *
     * The node must be embedded in an object that outlives its pending work.
     * Destruction while pending is a fatal lifecycle violation. A node can be
     * pending at most once, so repeated scheduling is idempotent and storage is
     * bounded by the number of live owning objects rather than a dynamic queue.
     */
    class DeferredCleanup final {
    public:
        using Function = void (*)(void*) noexcept;

        DeferredCleanup(void* context, Function function) noexcept;
        DeferredCleanup(const DeferredCleanup&) = delete;
        DeferredCleanup& operator=(const DeferredCleanup&) = delete;
        DeferredCleanup(DeferredCleanup&&) = delete;
        DeferredCleanup& operator=(DeferredCleanup&&) = delete;
        ~DeferredCleanup() noexcept;

        [[nodiscard]] bool pending() const noexcept;

    private:
        friend class EventLoop;

        void* context_;
        Function function_;
        DeferredCleanup* next_{nullptr};
        bool pending_{false};
    };

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

    /** Owner-thread-only one-shot timer scheduling and cancellation. */
    [[nodiscard]] Result<TimerId> run_after(
        std::chrono::steady_clock::duration delay,
        TimerCallback callback);
    [[nodiscard]] Result<TimerCancelOutcome> cancel_timer(TimerId id);

    [[nodiscard]] bool is_in_loop_thread() const noexcept;
    [[nodiscard]] State state() const noexcept;

    [[nodiscard]] Result<void> update_channel(Channel& channel);
    [[nodiscard]] Result<void> remove_channel(Channel& channel);
    [[nodiscard]] Result<void> queue_in_loop(Callback callback);

    /**
     * Schedules owner-thread-only framework cleanup after the active batch.
     *
     * Unlike queue_in_loop(), this intrusive path does not allocate and is not
     * subject to the ordinary pending callback capacity. It accepts work only
     * while the loop is Running or Stopping.
     */
    [[nodiscard]] Result<void> defer_cleanup(DeferredCleanup& cleanup);
    [[nodiscard]] bool dispatching_active_channels() const noexcept;

    [[nodiscard]] std::size_t pending_callback_count() const;
    [[nodiscard]] std::size_t pending_callback_capacity() const noexcept;
    [[nodiscard]] std::size_t logger_failure_count() const noexcept;

private:
    friend struct EventLoopTestAccess;
    friend class detail::TimerQueueTestAccess;

    EventLoop(
        ILogger& logger,
        std::unique_ptr<EpollPoller> poller,
        UniqueFd wakeup_fd,
        std::size_t pending_callback_capacity);

    [[nodiscard]] Result<void> initialize_wakeup_channel();
    [[nodiscard]] Result<void> initialize_timer_queue();
    [[nodiscard]] Result<void> signal_wakeup();
    void drain_wakeup() noexcept;
    void execute_pending_callbacks() noexcept;
    void execute_deferred_cleanups() noexcept;
    void finalize_timer_queue() noexcept;
    void handle_active_channels(const std::vector<Channel*>& active_channels) noexcept;
    void transition_to_stopped() noexcept;
    void safe_log(LogLevel level, const char* message) noexcept;

    ILogger& logger_;
    const std::thread::id owner_thread_;
    std::unique_ptr<EpollPoller> poller_;
    UniqueFd wakeup_fd_;
    std::unique_ptr<Channel> wakeup_channel_;
    std::unique_ptr<detail::TimerQueue> timer_queue_;
    const std::size_t pending_callback_capacity_;

    mutable std::mutex pending_mutex_;
    std::deque<Callback> pending_callbacks_;
    DeferredCleanup* deferred_cleanup_head_{nullptr};
    DeferredCleanup* deferred_cleanup_tail_{nullptr};
    std::atomic<State> state_{State::Created};
    std::atomic<std::size_t> logger_failure_count_{0U};
    bool dispatching_active_channels_{false};
};

}  // namespace iaisf::net
