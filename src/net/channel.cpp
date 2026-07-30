#include "iaisf/net/channel.hpp"

#include <cassert>
#include <utility>

#include "iaisf/net/event_loop.hpp"

namespace iaisf::net {

Channel::Channel(EventLoop& owner, const int fd) noexcept : owner_(owner), fd_(fd) {}

Channel::~Channel() noexcept {
    assert(!registered_ && "Channel must be removed before destruction");
}

int Channel::fd() const noexcept {
    return fd_;
}

std::uint32_t Channel::events() const noexcept {
    return events_;
}

std::uint32_t Channel::ready_events() const noexcept {
    return ready_events_;
}

bool Channel::has_events() const noexcept {
    return (events_ & ~kEdgeTriggered) != 0U;
}

bool Channel::is_registered() const noexcept {
    return registered_;
}

bool Channel::edge_triggered() const noexcept {
    return (events_ & kEdgeTriggered) != 0U;
}

void Channel::enable_reading() noexcept {
    events_ |= kReadEvents;
}

void Channel::disable_reading() noexcept {
    events_ &= ~kReadEvents;
}

void Channel::enable_writing() noexcept {
    events_ |= kWriteEvent;
}

void Channel::disable_writing() noexcept {
    events_ &= ~kWriteEvent;
}

void Channel::disable_all() noexcept {
    events_ &= kEdgeTriggered;
}

void Channel::set_edge_triggered(const bool enabled) noexcept {
    if (enabled) {
        events_ |= kEdgeTriggered;
    } else {
        events_ &= ~kEdgeTriggered;
    }
}

Result<void> Channel::update() {
    return owner_.update_channel(*this);
}

Result<void> Channel::remove() {
    return owner_.remove_channel(*this);
}

void Channel::set_read_callback(Callback callback) {
    read_callback_ = std::move(callback);
}

void Channel::set_write_callback(Callback callback) {
    write_callback_ = std::move(callback);
}

void Channel::set_error_callback(Callback callback) {
    error_callback_ = std::move(callback);
}

void Channel::set_close_callback(Callback callback) {
    close_callback_ = std::move(callback);
}

void Channel::set_ready_events(const std::uint32_t events) noexcept {
    ready_events_ = events;
}

void Channel::handle_event() {
    const bool has_read_side_event = (ready_events_ & kReadEvents) != 0U;
    if ((ready_events_ & kHangupEvent) != 0U &&
        !has_read_side_event &&
        close_callback_) {
        close_callback_();
    }
    if ((ready_events_ & kErrorEvent) != 0U && error_callback_) {
        error_callback_();
    }
    if (has_read_side_event && read_callback_) {
        read_callback_();
    }
    if ((ready_events_ & kWriteEvent) != 0U && write_callback_) {
        write_callback_();
    }
}

void Channel::set_registered(const bool registered) noexcept {
    registered_ = registered;
}

bool Channel::belongs_to(const EventLoop& loop) const noexcept {
    return &owner_ == &loop;
}

}  // namespace iaisf::net
