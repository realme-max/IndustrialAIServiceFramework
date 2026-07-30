#pragma once

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>

#include "iaisf/core/result.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net {

class Channel;

/**
 * Bounded Linux epoll wrapper used only by its owning EventLoop thread.
 *
 * Channels are non-owning pointers and must be removed before destruction.
 */
class EpollPoller final {
public:
    static constexpr std::size_t kMaximumEvents = 65'536U;

    [[nodiscard]] static Result<std::unique_ptr<EpollPoller>> create(
        std::size_t max_events);

    EpollPoller(const EpollPoller&) = delete;
    EpollPoller& operator=(const EpollPoller&) = delete;
    EpollPoller(EpollPoller&&) = delete;
    EpollPoller& operator=(EpollPoller&&) = delete;
    ~EpollPoller() = default;

    [[nodiscard]] Result<void> add(Channel& channel);
    [[nodiscard]] Result<void> update(Channel& channel);
    [[nodiscard]] Result<void> remove(Channel& channel);

    [[nodiscard]] Result<std::size_t> poll(
        int timeout_ms,
        std::vector<Channel*>& active_channels);

    [[nodiscard]] bool contains(const Channel& channel) const noexcept;
    [[nodiscard]] std::size_t registered_count() const noexcept;
    [[nodiscard]] std::size_t max_events() const noexcept;

private:
    EpollPoller(UniqueFd epoll_fd, std::size_t max_events);

    [[nodiscard]] Result<void> control(
        int operation,
        Channel& channel,
        const char* operation_name);

    UniqueFd epoll_fd_;
    std::vector<epoll_event> ready_events_;
    std::unordered_map<int, Channel*> channels_;
};

}  // namespace iaisf::net
