#include "iaisf/net/epoll_poller.hpp"

#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>

#include "iaisf/net/channel.hpp"
#include "system_error.hpp"

namespace iaisf::net {
namespace {

Result<void> invalid_state(const char* message) {
    return Result<void>::failure(make_error(ErrorCode::InvalidState, message));
}

}  // namespace

Result<std::unique_ptr<EpollPoller>> EpollPoller::create(
    const std::size_t max_events) {
    if (max_events == 0U || max_events > kMaximumEvents ||
        max_events > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Result<std::unique_ptr<EpollPoller>>::failure(
            make_error(ErrorCode::InvalidArgument, "epoll max_events is out of range"));
    }

    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        const int error_number = errno;
        return Result<std::unique_ptr<EpollPoller>>::failure(
            detail::make_system_error("epoll_create1", error_number));
    }

    auto poller = std::unique_ptr<EpollPoller>{
        new EpollPoller{UniqueFd{epoll_fd}, max_events}};
    return Result<std::unique_ptr<EpollPoller>>::success(std::move(poller));
}

EpollPoller::EpollPoller(UniqueFd epoll_fd, const std::size_t max_events)
    : epoll_fd_(std::move(epoll_fd)), ready_events_(max_events) {}

Result<void> EpollPoller::add(Channel& channel) {
    if (channel.fd() < 0) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "cannot add an invalid fd"));
    }
    if (!channel.has_events()) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "cannot add a channel without events"));
    }
    if (channels_.find(channel.fd()) != channels_.end() || channel.is_registered()) {
        return invalid_state("channel fd is already registered");
    }

    const auto [iterator, inserted] = channels_.emplace(channel.fd(), &channel);
    if (!inserted) {
        return invalid_state("channel fd is already registered");
    }

    auto result = control(EPOLL_CTL_ADD, channel, "epoll_ctl(ADD)");
    if (!result) {
        channels_.erase(iterator);
        return result;
    }
    channel.set_registered(true);
    return Result<void>::success();
}

Result<void> EpollPoller::update(Channel& channel) {
    const auto iterator = channels_.find(channel.fd());
    if (iterator == channels_.end() || !channel.is_registered()) {
        return invalid_state("channel fd is not registered");
    }
    if (iterator->second != &channel) {
        return invalid_state("registered fd belongs to a different channel");
    }
    if (!channel.has_events()) {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "cannot update to an empty event mask"));
    }
    return control(EPOLL_CTL_MOD, channel, "epoll_ctl(MOD)");
}

Result<void> EpollPoller::remove(Channel& channel) {
    const auto iterator = channels_.find(channel.fd());
    if (iterator == channels_.end() || !channel.is_registered()) {
        return invalid_state("channel fd is not registered");
    }
    if (iterator->second != &channel) {
        return invalid_state("registered fd belongs to a different channel");
    }

    auto result = control(EPOLL_CTL_DEL, channel, "epoll_ctl(DEL)");
    if (!result) {
        return result;
    }
    channels_.erase(iterator);
    channel.set_registered(false);
    channel.set_ready_events(0U);
    return Result<void>::success();
}

Result<std::size_t> EpollPoller::poll(
    const int timeout_ms,
    std::vector<Channel*>& active_channels) {
    active_channels.clear();
    if (timeout_ms < -1) {
        return Result<std::size_t>::failure(
            make_error(ErrorCode::InvalidArgument, "epoll timeout is out of range"));
    }

    const int event_count = ::epoll_wait(
        epoll_fd_.get(),
        ready_events_.data(),
        static_cast<int>(ready_events_.size()),
        timeout_ms);
    if (event_count < 0) {
        const int error_number = errno;
        if (error_number == EINTR) {
            return Result<std::size_t>::success(0U);
        }
        return Result<std::size_t>::failure(
            detail::make_system_error("epoll_wait", error_number));
    }

    active_channels.reserve(static_cast<std::size_t>(event_count));
    for (int index = 0; index < event_count; ++index) {
        const epoll_event& event = ready_events_[static_cast<std::size_t>(index)];
        const auto iterator = channels_.find(event.data.fd);
        if (iterator == channels_.end() || iterator->second == nullptr) {
            active_channels.clear();
            return Result<std::size_t>::failure(
                make_error(ErrorCode::InvalidState, "epoll returned an unknown fd"));
        }

        Channel* const channel = iterator->second;
        channel->set_ready_events(event.events);
        active_channels.push_back(channel);
    }

    return Result<std::size_t>::success(active_channels.size());
}

bool EpollPoller::contains(const Channel& channel) const noexcept {
    const auto iterator = channels_.find(channel.fd());
    return iterator != channels_.end() && iterator->second == &channel;
}

std::size_t EpollPoller::registered_count() const noexcept {
    return channels_.size();
}

std::size_t EpollPoller::max_events() const noexcept {
    return ready_events_.size();
}

Result<void> EpollPoller::control(
    const int operation,
    Channel& channel,
    const char* const operation_name) {
    epoll_event event{};
    event.events = channel.events();
    event.data.fd = channel.fd();
    if (::epoll_ctl(epoll_fd_.get(), operation, channel.fd(), &event) < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            detail::make_system_error(operation_name, error_number));
    }
    return Result<void>::success();
}

}  // namespace iaisf::net
