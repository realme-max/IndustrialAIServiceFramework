#include "iaisf/net/tcp/ipv4_endpoint.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <new>
#include <string>
#include <utility>

#include "../system_error.hpp"
#include "iaisf/core/error.hpp"

namespace iaisf::net::tcp {

Ipv4Endpoint::Ipv4Endpoint(
    const std::uint32_t address_network_order,
    const std::uint16_t port) noexcept
    : address_network_order_(address_network_order), port_(port) {}

Result<Ipv4Endpoint> Ipv4Endpoint::from_string(
    const std::string_view address,
    const std::uint16_t port) {
    if (address.empty()) {
        return Result<Ipv4Endpoint>::failure(make_error(
            ErrorCode::InvalidArgument,
            "IPv4 address must not be empty"));
    }

    try {
        const std::string terminated_address{address};
        in_addr parsed{};
        const int parse_result =
            ::inet_pton(AF_INET, terminated_address.c_str(), &parsed);
        if (parse_result == 1) {
            return Result<Ipv4Endpoint>::success(
                Ipv4Endpoint{parsed.s_addr, port});
        }
        if (parse_result == 0) {
            return Result<Ipv4Endpoint>::failure(make_error(
                ErrorCode::InvalidArgument,
                "invalid numeric IPv4 address"));
        }
        const int error_number = errno;
        return Result<Ipv4Endpoint>::failure(detail::make_system_error(
            "inet_pton",
            error_number));
    } catch (const std::bad_alloc&) {
        return Result<Ipv4Endpoint>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "memory allocation failed while parsing IPv4 address"));
    }
}

Ipv4Endpoint Ipv4Endpoint::loopback(const std::uint16_t port) noexcept {
    return Ipv4Endpoint{::htonl(INADDR_LOOPBACK), port};
}

Ipv4Endpoint Ipv4Endpoint::any(const std::uint16_t port) noexcept {
    return Ipv4Endpoint{::htonl(INADDR_ANY), port};
}

Result<Ipv4Endpoint> Ipv4Endpoint::from_sockaddr(
    const sockaddr_in& address) {
    if (address.sin_family != AF_INET) {
        return Result<Ipv4Endpoint>::failure(make_error(
            ErrorCode::InvalidArgument,
            "socket address family is not AF_INET"));
    }
    return Result<Ipv4Endpoint>::success(Ipv4Endpoint{
        address.sin_addr.s_addr,
        ::ntohs(address.sin_port)});
}

sockaddr_in Ipv4Endpoint::to_sockaddr() const noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port_);
    address.sin_addr.s_addr = address_network_order_;
    return address;
}

Result<std::string> Ipv4Endpoint::address_string() const {
    char buffer[INET_ADDRSTRLEN]{};
    in_addr address{};
    address.s_addr = address_network_order_;
    if (::inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) == nullptr) {
        const int error_number = errno;
        return Result<std::string>::failure(detail::make_system_error(
            "inet_ntop",
            error_number));
    }

    try {
        return Result<std::string>::success(std::string{buffer});
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "memory allocation failed while formatting IPv4 address"));
    }
}

Result<std::string> Ipv4Endpoint::to_string() const {
    auto address_result = address_string();
    if (!address_result) {
        return Result<std::string>::failure(address_result.error());
    }

    try {
        std::string formatted = std::move(address_result).value();
        formatted.push_back(':');
        formatted += std::to_string(port_);
        return Result<std::string>::success(std::move(formatted));
    } catch (const std::bad_alloc&) {
        return Result<std::string>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "memory allocation failed while formatting IPv4 endpoint"));
    }
}

std::uint16_t Ipv4Endpoint::port() const noexcept {
    return port_;
}

std::uint32_t Ipv4Endpoint::address_network_order() const noexcept {
    return address_network_order_;
}

bool operator==(
    const Ipv4Endpoint& left,
    const Ipv4Endpoint& right) noexcept {
    return left.address_network_order_ == right.address_network_order_ &&
           left.port_ == right.port_;
}

bool operator!=(
    const Ipv4Endpoint& left,
    const Ipv4Endpoint& right) noexcept {
    return !(left == right);
}

}  // namespace iaisf::net::tcp
