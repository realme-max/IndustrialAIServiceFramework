#include <cstdint>
#include <string>

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "iaisf/core/error.hpp"
#include "iaisf/net/tcp/ipv4_endpoint.hpp"

namespace {

using iaisf::net::tcp::Ipv4Endpoint;

TEST(Ipv4EndpointTest, FormatsLoopbackAnyAndPortBoundaries) {
    const auto loopback = Ipv4Endpoint::loopback(0U);
    const auto any = Ipv4Endpoint::any(65'535U);

    auto loopback_text = loopback.to_string();
    auto any_text = any.to_string();

    ASSERT_TRUE(loopback_text);
    ASSERT_TRUE(any_text);
    EXPECT_EQ(loopback_text.value(), "127.0.0.1:0");
    EXPECT_EQ(any_text.value(), "0.0.0.0:65535");
}

TEST(Ipv4EndpointTest, NumericAddressRoundTripsThroughSockaddr) {
    auto parsed = Ipv4Endpoint::from_string("192.0.2.10", 8080U);
    ASSERT_TRUE(parsed);

    const sockaddr_in native = parsed.value().to_sockaddr();
    auto round_trip = Ipv4Endpoint::from_sockaddr(native);

    ASSERT_TRUE(round_trip);
    EXPECT_EQ(round_trip.value(), parsed.value());
    EXPECT_EQ(round_trip.value().port(), 8080U);
}

TEST(Ipv4EndpointTest, RejectsInvalidAddressAndDnsName) {
    auto malformed = Ipv4Endpoint::from_string("999.1.2.3", 80U);
    auto dns_name = Ipv4Endpoint::from_string("localhost", 80U);
    auto empty = Ipv4Endpoint::from_string("", 80U);

    ASSERT_FALSE(malformed);
    ASSERT_FALSE(dns_name);
    ASSERT_FALSE(empty);
    EXPECT_EQ(malformed.error().code, iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(dns_name.error().code, iaisf::ErrorCode::InvalidArgument);
    EXPECT_EQ(empty.error().code, iaisf::ErrorCode::InvalidArgument);
}

TEST(Ipv4EndpointTest, RejectsNonIpv4SockaddrFamily) {
    sockaddr_in address{};
    address.sin_family = AF_UNIX;

    auto result = Ipv4Endpoint::from_sockaddr(address);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, iaisf::ErrorCode::InvalidArgument);
}

}  // namespace
