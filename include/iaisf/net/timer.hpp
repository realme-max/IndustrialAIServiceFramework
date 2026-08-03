#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "iaisf/core/result.hpp"

namespace iaisf::net {

namespace detail {
class TimerQueue;
class TimerQueueTestAccess;
struct TimerIdLess;
}  // namespace detail

/** Opaque, process-local handle for a timer. */
class TimerId final {
public:
    constexpr TimerId() noexcept = default;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return sequence_ != 0U;
    }

    friend constexpr bool operator==(
        const TimerId lhs,
        const TimerId rhs) noexcept {
        return lhs.sequence_ == rhs.sequence_;
    }

    friend constexpr bool operator!=(
        const TimerId lhs,
        const TimerId rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    explicit constexpr TimerId(const std::uint64_t sequence) noexcept
        : sequence_(sequence) {}

    std::uint64_t sequence_{0U};

    friend class detail::TimerQueue;
    friend class detail::TimerQueueTestAccess;
    friend struct detail::TimerIdLess;
};

using TimerCallback = std::function<void()>;

enum class TimerCancelOutcome {
    Cancelled,
    TooLate,
    NotFound,
};

struct TimerQueueOptions {
    static constexpr std::size_t kDefaultMaxTimers = 1024U;
    static constexpr std::size_t kHardMaxTimers = 1'000'000U;

    std::size_t max_timers{kDefaultMaxTimers};

    [[nodiscard]] static Result<TimerQueueOptions> create(
        std::size_t max_timers);
};

}  // namespace iaisf::net
