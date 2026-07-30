#pragma once

#include <cstdint>
#include <functional>
#include <type_traits>

#include <sys/epoll.h>

#include "iaisf/core/result.hpp"

namespace iaisf::net {

class EpollPoller;
class EventLoop;

/**
 * Non-owning fd event view bound to one EventLoop.
 *
 * The fd and EventLoop must outlive the Channel. Once registered, the Channel
 * address must remain stable and remove() must succeed before destruction.
 * A Channel returned in an active event batch must remain alive until the
 * entire batch has been dispatched. Application callbacks use
 * EventLoop::queue_in_loop(); framework lifecycle owners use the separate
 * EventLoop deferred-cleanup path.
 * Callback mutation and event-mask mutation are owner-thread-only operations.
 */
class Channel final {
public:
    using Callback = std::function<void()>;

    static constexpr std::uint32_t kReadEvents =
        static_cast<std::uint32_t>(EPOLLIN) |
        static_cast<std::uint32_t>(EPOLLPRI) |
        static_cast<std::uint32_t>(EPOLLRDHUP);
    static constexpr std::uint32_t kWriteEvent =
        static_cast<std::uint32_t>(EPOLLOUT);
    static constexpr std::uint32_t kErrorEvent =
        static_cast<std::uint32_t>(EPOLLERR);
    static constexpr std::uint32_t kHangupEvent =
        static_cast<std::uint32_t>(EPOLLHUP);
    static constexpr std::uint32_t kEdgeTriggered =
        static_cast<std::uint32_t>(EPOLLET);

    Channel(EventLoop& owner, int fd) noexcept;
    ~Channel() noexcept;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] std::uint32_t events() const noexcept;
    [[nodiscard]] std::uint32_t ready_events() const noexcept;
    [[nodiscard]] bool has_events() const noexcept;
    [[nodiscard]] bool is_registered() const noexcept;
    [[nodiscard]] bool edge_triggered() const noexcept;

    void enable_reading() noexcept;
    void disable_reading() noexcept;
    void enable_writing() noexcept;
    void disable_writing() noexcept;
    void disable_all() noexcept;
    void set_edge_triggered(bool enabled) noexcept;

    [[nodiscard]] Result<void> update();
    [[nodiscard]] Result<void> remove();

    void set_read_callback(Callback callback);
    void set_write_callback(Callback callback);
    void set_error_callback(Callback callback);
    void set_close_callback(Callback callback);

    /**
     * Stores events returned by a Poller. Public for deterministic component
     * tests; production callers must leave this to EpollPoller.
     */
    void set_ready_events(std::uint32_t events) noexcept;

    /**
     * Dispatches HUP-only close, error, read-side, then writable events.
     *
     * EPOLLRDHUP is a read-side notification. EPOLLHUP only closes directly
     * when no read-side event is present, so HUP plus readable data can be
     * drained by the future connection layer. Channel only notifies callbacks;
     * it never drains an edge-triggered fd itself.
     *
     * An exception stops the remaining callbacks for this Channel. EventLoop
     * catches it at the per-Channel boundary and continues the active batch.
     */
    void handle_event();

private:
    friend class EpollPoller;
    friend class EventLoop;

    void set_registered(bool registered) noexcept;
    [[nodiscard]] bool belongs_to(const EventLoop& loop) const noexcept;

    EventLoop& owner_;
    int fd_;
    std::uint32_t events_{0U};
    std::uint32_t ready_events_{0U};
    bool registered_{false};
    Callback read_callback_;
    Callback write_callback_;
    Callback error_callback_;
    Callback close_callback_;
};

static_assert(!std::is_copy_constructible_v<Channel>);
static_assert(!std::is_copy_assignable_v<Channel>);
static_assert(!std::is_move_constructible_v<Channel>);
static_assert(!std::is_move_assignable_v<Channel>);

}  // namespace iaisf::net
