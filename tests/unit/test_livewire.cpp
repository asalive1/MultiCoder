// test_livewire.cpp — Unit tests for LivewireMapping
// Tests every case from the spec: channel numbers 0,1,255,256,257,14101,65535
// plus roundtrip tests and boundary/error cases.

#include <catch2/catch_test_macros.hpp>
#include "LivewireMapping.h"

using namespace livewire;

TEST_CASE("channelToMulticast — known values", "[livewire]") {
    CHECK(channelToMulticast(0)     == "239.192.0.0");
    CHECK(channelToMulticast(1)     == "239.192.0.1");
    CHECK(channelToMulticast(255)   == "239.192.0.255");
    CHECK(channelToMulticast(256)   == "239.192.1.0");
    CHECK(channelToMulticast(257)   == "239.192.1.1");
    // N=14101: A=14101/256=55, B=14101%256=21  => 239.192.55.21
    CHECK(channelToMulticast(14101) == "239.192.55.21");
    CHECK(channelToMulticast(65535) == "239.192.255.255");
}

TEST_CASE("channelToMulticast — out of range", "[livewire]") {
    CHECK_FALSE(channelToMulticast(65536).has_value());
    CHECK_FALSE(channelToMulticast(100000).has_value());
}

TEST_CASE("multicastToChannel — known values", "[livewire]") {
    CHECK(multicastToChannel("239.192.0.0")    == 0u);
    CHECK(multicastToChannel("239.192.0.1")    == 1u);
    CHECK(multicastToChannel("239.192.0.255")  == 255u);
    CHECK(multicastToChannel("239.192.1.0")    == 256u);
    CHECK(multicastToChannel("239.192.1.1")    == 257u);
    CHECK(multicastToChannel("239.192.55.21")  == 14101u);
    CHECK(multicastToChannel("239.192.255.255")== 65535u);
}

TEST_CASE("multicastToChannel — wrong prefix rejected", "[livewire]") {
    CHECK_FALSE(multicastToChannel("239.193.0.0").has_value());
    CHECK_FALSE(multicastToChannel("239.191.0.0").has_value());
    CHECK_FALSE(multicastToChannel("192.168.1.1").has_value());
    CHECK_FALSE(multicastToChannel("").has_value());
    CHECK_FALSE(multicastToChannel("not.an.ip").has_value());
}

TEST_CASE("channelToMulticast/multicastToChannel — roundtrip", "[livewire]") {
    for (uint32_t n : {0u, 1u, 255u, 256u, 257u, 14101u, 65535u}) {
        auto ip = channelToMulticast(n);
        REQUIRE(ip.has_value());
        auto back = multicastToChannel(*ip);
        REQUIRE(back.has_value());
        CHECK(*back == n);
    }
}

TEST_CASE("LIVEWIRE_RTP_PORT constant", "[livewire]") {
    CHECK(LIVEWIRE_RTP_PORT == 5004);
}
