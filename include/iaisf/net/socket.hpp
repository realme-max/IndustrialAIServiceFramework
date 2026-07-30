#pragma once

#include <type_traits>

#include "iaisf/core/result.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net {

/**
 * Minimal owner for a Linux IPv4 TCP socket.
 *
 * The class does not bind, listen, accept, connect, perform DNS resolution,
 * or create threads. Ordinary operations are not thread-safe.
 */
class Socket final {
public:
    Socket() noexcept = default;
    explicit Socket(UniqueFd fd) noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;
    ~Socket() = default;

    [[nodiscard]] static Result<Socket> create_ipv4_tcp();

    [[nodiscard]] int native_handle() const noexcept;
    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

    [[nodiscard]] UniqueFd release() noexcept;
    void reset(UniqueFd fd = UniqueFd{}) noexcept;

    [[nodiscard]] Result<void> set_reuse_address(bool enabled = true) const;
    [[nodiscard]] Result<void> shutdown_write() const;

private:
    UniqueFd fd_;
};

static_assert(!std::is_copy_constructible_v<Socket>);
static_assert(!std::is_copy_assignable_v<Socket>);
static_assert(std::is_nothrow_move_constructible_v<Socket>);
static_assert(std::is_nothrow_move_assignable_v<Socket>);

}  // namespace iaisf::net
