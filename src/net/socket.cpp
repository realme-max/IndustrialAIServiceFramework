#include "iaisf/net/socket.hpp"

#include <cerrno>
#include <utility>

#include <netinet/in.h>
#include <sys/socket.h>

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

Result<void> Socket::shutdown_write() const {
    if (::shutdown(fd_.get(), SHUT_WR) < 0) {
        const int error_number = errno;
        return Result<void>::failure(
            detail::make_system_error("shutdown(SHUT_WR)", error_number));
    }
    return Result<void>::success();
}

}  // namespace iaisf::net
