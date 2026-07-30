#pragma once

#include <type_traits>
#include <utility>

#include <unistd.h>

namespace iaisf::net {

/**
 * Move-only owner for a Linux file descriptor.
 *
 * close() is attempted at most once. In particular, EINTR is not retried
 * because Linux may already have released and reused the descriptor number.
 */
class UniqueFd final {
public:
    static constexpr int kInvalid = -1;

    constexpr UniqueFd() noexcept = default;
    explicit constexpr UniqueFd(const int fd) noexcept : fd_(fd) {}

    ~UniqueFd() noexcept {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] constexpr int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return fd_ != kInvalid;
    }

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, kInvalid);
    }

    void reset(const int new_fd = kInvalid) noexcept {
        if (fd_ == new_fd) {
            return;
        }

        const int old_fd = std::exchange(fd_, new_fd);
        if (old_fd != kInvalid) {
            static_cast<void>(::close(old_fd));
        }
    }

    void swap(UniqueFd& other) noexcept {
        std::swap(fd_, other.fd_);
    }

private:
    int fd_{kInvalid};
};

inline void swap(UniqueFd& lhs, UniqueFd& rhs) noexcept {
    lhs.swap(rhs);
}

static_assert(!std::is_copy_constructible_v<UniqueFd>);
static_assert(!std::is_copy_assignable_v<UniqueFd>);
static_assert(std::is_nothrow_move_constructible_v<UniqueFd>);
static_assert(std::is_nothrow_move_assignable_v<UniqueFd>);
static_assert(std::is_nothrow_destructible_v<UniqueFd>);

}  // namespace iaisf::net
