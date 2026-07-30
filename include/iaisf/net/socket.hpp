#pragma once

#include <optional>
#include <type_traits>

#include "iaisf/core/result.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"
#include "iaisf/net/unique_fd.hpp"

namespace iaisf::net {

/**
 * Move-only owner for a nonblocking Linux IPv4 TCP socket.
 *
 * The class performs no DNS resolution and creates no threads. Ordinary
 * operations are owner-thread-only.
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
    [[nodiscard]] Result<void> set_no_delay(bool enabled = true) const;
    [[nodiscard]] Result<void> set_send_buffer_size(int bytes) const;
    [[nodiscard]] Result<void> bind(
        const tcp::Ipv4Endpoint& endpoint) const;
    [[nodiscard]] Result<void> listen(int backlog) const;
    [[nodiscard]] Result<tcp::Ipv4Endpoint> local_endpoint() const;
    [[nodiscard]] Result<int> socket_error() const;
    [[nodiscard]] Result<void> shutdown_write() const;

private:
    UniqueFd fd_;
};

static_assert(!std::is_copy_constructible_v<Socket>);
static_assert(!std::is_copy_assignable_v<Socket>);
static_assert(std::is_nothrow_move_constructible_v<Socket>);
static_assert(std::is_nothrow_move_assignable_v<Socket>);

struct AcceptedSocket final {
    Socket socket;
    tcp::Ipv4Endpoint peer_endpoint;
};

/**
 * Accepts one nonblocking close-on-exec IPv4 client.
 *
 * EINTR and transient ECONNABORTED are retried. EAGAIN/EWOULDBLOCK is returned
 * as a successful empty optional so an edge-triggered accept loop can stop.
 */
[[nodiscard]] Result<std::optional<AcceptedSocket>> accept_ipv4(
    const Socket& listening_socket);

}  // namespace iaisf::net
