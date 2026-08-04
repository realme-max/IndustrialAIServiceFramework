#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <signal.h>

#include "iaisf/core/result.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net {

class Channel;
class EventLoop;

namespace detail {

/**
 * Linux owner-thread signal source embedded in one EventLoop.
 *
 * The queue exclusively owns its signalfd and Channel. It also tracks only the
 * SIGINT/SIGTERM mask bits added by this instance, leaving pre-existing caller
 * mask state untouched during shutdown.
 */
class SignalQueue final {
public:
    using Callback = std::function<void()>;
    using ErrorNotification = std::function<void(const char*)>;

    [[nodiscard]] static Result<std::unique_ptr<SignalQueue>> create(
        EventLoop& owner,
        Callback before_loop_stop,
        ErrorNotification error_notification);

    SignalQueue(const SignalQueue&) = delete;
    SignalQueue& operator=(const SignalQueue&) = delete;
    SignalQueue(SignalQueue&&) = delete;
    SignalQueue& operator=(SignalQueue&&) = delete;
    ~SignalQueue() noexcept;

    [[nodiscard]] Result<void> shutdown();

private:
    SignalQueue(
        EventLoop& owner,
        Callback before_loop_stop,
        ErrorNotification error_notification) noexcept;

    [[nodiscard]] Result<void> block_signals();
    [[nodiscard]] Result<void> initialize_descriptor();
    [[nodiscard]] Result<void> initialize_channel();
    [[nodiscard]] Result<void> drain_descriptor(bool request_stop);
    [[nodiscard]] Result<void> restore_owned_mask();
    void handle_readable() noexcept;
    void handle_descriptor_failure() noexcept;
    void request_shutdown(const char* message) noexcept;
    void notify_error(const char* message) noexcept;

    EventLoop& owner_;
    Callback before_loop_stop_;
    ErrorNotification error_notification_;
    UniqueFd signal_fd_;
    std::unique_ptr<Channel> signal_channel_;
    sigset_t handled_mask_{};
    sigset_t owned_mask_{};
    bool mask_installed_{false};
    bool mask_restore_safe_{true};
    bool shutdown_requested_{false};
    bool shutdown_{false};

    friend class SignalQueueTestAccess;
};

/** Test-only observer for the internal signalfd source. */
class SignalQueueTestAccess final {
public:
    [[nodiscard]] static const SignalQueue* queue(
        const EventLoop& loop) noexcept;
    [[nodiscard]] static int signal_fd(const SignalQueue& queue) noexcept;
    [[nodiscard]] static bool channel_registered(
        const SignalQueue& queue) noexcept;
    [[nodiscard]] static bool channel_edge_triggered(
        const SignalQueue& queue) noexcept;
    [[nodiscard]] static bool shutdown_requested(
        const SignalQueue& queue) noexcept;
};

}  // namespace detail
}  // namespace iaisf::net
