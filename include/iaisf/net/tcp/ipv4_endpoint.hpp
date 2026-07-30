#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <netinet/in.h>

#include "iaisf/core/result.hpp"

namespace iaisf::net::tcp {

/**
 * IPv4 address and host-order port value object.
 *
 * The type performs numeric IPv4 parsing only. It never performs DNS
 * resolution and owns no operating-system resource.
 */
class Ipv4Endpoint final {
public:
    [[nodiscard]] static Result<Ipv4Endpoint> from_string(
        std::string_view address,
        std::uint16_t port);
    [[nodiscard]] static Ipv4Endpoint loopback(std::uint16_t port) noexcept;
    [[nodiscard]] static Ipv4Endpoint any(std::uint16_t port) noexcept;
    [[nodiscard]] static Result<Ipv4Endpoint> from_sockaddr(
        const sockaddr_in& address);

    [[nodiscard]] sockaddr_in to_sockaddr() const noexcept;
    [[nodiscard]] Result<std::string> address_string() const;
    [[nodiscard]] Result<std::string> to_string() const;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::uint32_t address_network_order() const noexcept;

    friend bool operator==(
        const Ipv4Endpoint& left,
        const Ipv4Endpoint& right) noexcept;
    friend bool operator!=(
        const Ipv4Endpoint& left,
        const Ipv4Endpoint& right) noexcept;

private:
    Ipv4Endpoint(
        std::uint32_t address_network_order,
        std::uint16_t port) noexcept;

    std::uint32_t address_network_order_{0U};
    std::uint16_t port_{0U};
};

}  // namespace iaisf::net::tcp
