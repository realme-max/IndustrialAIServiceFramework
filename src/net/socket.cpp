#include "iaisf/net/socket.hpp"

#include <cerrno>
#include <optional>
#include <utility>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "iaisf/core/error.hpp"
#include "system_error.hpp"

namespace iaisf::net {

Socket::Socket(UniqueFd fd) noexcept : fd_(std::move(fd)) {}

Result<Socket> Socket::create_ipv4_tcp() {
    const int socket_fd =
        ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
    if (socket_fd < 0) {
        const int error_number = errno;
        return Result<Socket>::failure(
            detail::make_system_error("socket", error_number));
    }

    return Result<Socket>::success(Socket{UniqueFd{socket_fd}});
}

int Socket::native_handle() const noexcept {
    return fd_.get();
}

int Socket::get() const noexcept {
    return native_handle();
}

bool Socket::valid() const noexcept {
    return fd_.valid();
}

Socket::operator bool() const noexcept {
    return valid();
}

UniqueFd Socket::release() noexcept {
    return UniqueFd{fd_.release()};
}

void Socket::reset(UniqueFd fd) noexcept {
    fd_ = std::move(fd);
}

Result<void> Socket::set_reuse_address(const bool enabled) const {
    const int option_value = enabled ? 1 : 0;
    if (::setsockopt(
            fd_.get(),
            SOL_SOCKET,
            SO_REUSEADDR,
            &option_value,
            static_cast<socklen_t>(sizeof(option_value))) < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            detail::make_system_error("setsockopt(SO_REUSEADDR)", error_number));
    }
    return Result<void>::success();
}

Result<void> Socket::set_no_delay(const bool enabled) const {
    const int option_value = enabled ? 1 : 0;
    if (::setsockopt(
            fd_.get(),
            IPPROTO_TCP,
            TCP_NODELAY,
            &option_value,
            static_cast<socklen_t>(sizeof(option_value))) < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            detail::make_system_error("setsockopt(TCP_NODELAY)", error_number));
    }
    return Result<void>::success();
}

Result<void> Socket::set_send_buffer_size(const int bytes) const {
    if (bytes <= 0) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "socket send buffer size must be greater than zero"));
    }
    if (::setsockopt(
            fd_.get(),
            SOL_SOCKET,
            SO_SNDBUF,
            &bytes,
            static_cast<socklen_t>(sizeof(bytes))) < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            detail::make_system_error("setsockopt(SO_SNDBUF)", error_number));
    }
    return Result<void>::success();
}

Result<void> Socket::bind(const tcp::Ipv4Endpoint& endpoint) const {
    const sockaddr_in address = endpoint.to_sockaddr();
    if (::bind(
            fd_.get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<socklen_t>(sizeof(address))) < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            detail::make_system_error("bind", error_number));
    }
    return Result<void>::success();
}

Result<void> Socket::listen(const int backlog) const {
    if (backlog <= 0) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidArgument,
            "listen backlog must be greater than zero"));
    }
    if (::listen(fd_.get(), backlog) < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            detail::make_system_error("listen", error_number));
    }
    return Result<void>::success();
}

Result<tcp::Ipv4Endpoint> Socket::local_endpoint() const {
    sockaddr_in address{};
    socklen_t length = static_cast<socklen_t>(sizeof(address));
    if (::getsockname(
            fd_.get(),
            reinterpret_cast<sockaddr*>(&address),
            &length) < 0) {
        const int error_number = errno;
        return Result<tcp::Ipv4Endpoint>::failure(
            detail::make_system_error("getsockname", error_number));
    }
    if (length != static_cast<socklen_t>(sizeof(address))) {
        return Result<tcp::Ipv4Endpoint>::failure(make_error(
            ErrorCode::SystemError,
            "getsockname returned an unexpected IPv4 address length"));
    }
    return tcp::Ipv4Endpoint::from_sockaddr(address);
}

Result<int> Socket::socket_error() const {
    int error_value = 0;
    socklen_t length = static_cast<socklen_t>(sizeof(error_value));
    if (::getsockopt(
            fd_.get(),
            SOL_SOCKET,
            SO_ERROR,
            &error_value,
            &length) < 0) {
        const int error_number = errno;
        return Result<int>::failure(
            detail::make_system_error("getsockopt(SO_ERROR)", error_number));
    }
    if (length != static_cast<socklen_t>(sizeof(error_value))) {
        return Result<int>::failure(make_error(
            ErrorCode::SystemError,
            "getsockopt(SO_ERROR) returned an unexpected value length"));
    }
    return Result<int>::success(error_value);
}

Result<void> Socket::shutdown_write() const {
    if (::shutdown(fd_.get(), SHUT_WR) < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            detail::make_system_error("shutdown(SHUT_WR)", error_number));
    }
    return Result<void>::success();
}

Result<std::optional<AcceptedSocket>> accept_ipv4(
    const Socket& listening_socket) {
    for (;;) {
        sockaddr_in peer_address{};
        socklen_t peer_length =
            static_cast<socklen_t>(sizeof(peer_address));
        const int descriptor = ::accept4(
            listening_socket.native_handle(),
            reinterpret_cast<sockaddr*>(&peer_address),
            &peer_length,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (descriptor >= 0) {
            Socket accepted_socket{UniqueFd{descriptor}};
            if (peer_length !=
                static_cast<socklen_t>(sizeof(peer_address))) {
                return Result<std::optional<AcceptedSocket>>::failure(
                    make_error(
                        ErrorCode::SystemError,
                        "accept4 returned an unexpected IPv4 address length"));
            }
            auto endpoint_result =
                tcp::Ipv4Endpoint::from_sockaddr(peer_address);
            if (!endpoint_result) {
                return Result<std::optional<AcceptedSocket>>::failure(
                    endpoint_result.error());
            }
            return Result<std::optional<AcceptedSocket>>::success(
                std::optional<AcceptedSocket>{AcceptedSocket{
                    std::move(accepted_socket),
                    std::move(endpoint_result).value()}});
        }

        const int error_number = errno;
        if (error_number == EINTR || error_number == ECONNABORTED) {
            continue;
        }
        if (error_number == EAGAIN || error_number == EWOULDBLOCK) {
            return Result<std::optional<AcceptedSocket>>::success(
                std::nullopt);
        }
        return Result<std::optional<AcceptedSocket>>::failure(
            detail::make_system_error("accept4", error_number));
    }
}

}  // namespace iaisf::net
