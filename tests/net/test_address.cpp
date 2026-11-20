// net/address.h against Python 3.12.10's ipaddress. The expected values in kCases were printed by
// that interpreter's ip_address() (str(), is_loopback, is_private, is_link_local, is_global and
// ipv4_mapped or the address), so the table is Python's answer, not a reading of its source.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>

#include "base/cancel.h"
#include "net/address.h"

using namespace fv;
using namespace fv::net;

namespace {

struct AddressCase {
    const char* text;
    bool valid;
    const char* str;
    bool loopback;
    bool private_;
    bool link_local;
    bool global;
    const char* unmapped;
};

const AddressCase kCases[] = {
    {"127.0.0.1", true, "127.0.0.1", true, true, false, false, "127.0.0.1"},
    {"127.255.255.254", true, "127.255.255.254", true, true, false, false, "127.255.255.254"},
    {"10.1.2.3", true, "10.1.2.3", false, true, false, false, "10.1.2.3"},
    {"172.16.0.1", true, "172.16.0.1", false, true, false, false, "172.16.0.1"},
    {"172.32.0.1", true, "172.32.0.1", false, false, false, true, "172.32.0.1"},
    {"192.168.1.20", true, "192.168.1.20", false, true, false, false, "192.168.1.20"},
    {"169.254.1.1", true, "169.254.1.1", false, true, true, false, "169.254.1.1"},
    {"100.64.0.1", true, "100.64.0.1", false, false, false, false, "100.64.0.1"},
    {"100.127.255.254", true, "100.127.255.254", false, false, false, false, "100.127.255.254"},
    {"100.128.0.1", true, "100.128.0.1", false, false, false, true, "100.128.0.1"},
    {"8.8.8.8", true, "8.8.8.8", false, false, false, true, "8.8.8.8"},
    {"1.1.1.1", true, "1.1.1.1", false, false, false, true, "1.1.1.1"},
    {"0.0.0.0", true, "0.0.0.0", false, true, false, false, "0.0.0.0"},
    {"255.255.255.255", true, "255.255.255.255", false, true, false, false, "255.255.255.255"},
    {"192.0.0.9", true, "192.0.0.9", false, false, false, true, "192.0.0.9"},
    {"192.0.0.10", true, "192.0.0.10", false, false, false, true, "192.0.0.10"},
    {"192.0.0.8", true, "192.0.0.8", false, true, false, false, "192.0.0.8"},
    {"192.0.2.1", true, "192.0.2.1", false, true, false, false, "192.0.2.1"},
    {"198.18.0.1", true, "198.18.0.1", false, true, false, false, "198.18.0.1"},
    {"203.0.113.7", true, "203.0.113.7", false, true, false, false, "203.0.113.7"},
    {"240.0.0.1", true, "240.0.0.1", false, true, false, false, "240.0.0.1"},
    {"224.0.0.1", true, "224.0.0.1", false, false, false, true, "224.0.0.1"},
    {"::1", true, "::1", true, true, false, false, "::1"},
    {"::", true, "::", false, true, false, false, "::"},
    {"fe80::1", true, "fe80::1", false, true, true, false, "fe80::1"},
    {"fe80::1%3", true, "fe80::1%3", false, true, true, false, "fe80::1%3"},
    {"fe80::1%eth0", true, "fe80::1%eth0", false, true, true, false, "fe80::1%eth0"},
    {"fd7a:115c:a1e0::1", true, "fd7a:115c:a1e0::1", false, true, false, false,
     "fd7a:115c:a1e0::1"},
    {"fc00::1", true, "fc00::1", false, true, false, false, "fc00::1"},
    {"2001:db8::1", true, "2001:db8::1", false, true, false, false, "2001:db8::1"},
    {"2001:4860:4860::8888", true, "2001:4860:4860::8888", false, false, false, true,
     "2001:4860:4860::8888"},
    {"2606:4700:4700::1111", true, "2606:4700:4700::1111", false, false, false, true,
     "2606:4700:4700::1111"},
    {"::ffff:127.0.0.1", true, "::ffff:7f00:1", true, true, false, false, "127.0.0.1"},
    {"::ffff:8.8.8.8", true, "::ffff:808:808", false, false, false, true, "8.8.8.8"},
    {"::ffff:100.64.0.2", true, "::ffff:6440:2", false, false, false, false, "100.64.0.2"},
    {"::ffff:192.168.0.1", true, "::ffff:c0a8:1", false, true, false, false, "192.168.0.1"},
    {"::ffff:169.254.0.1", true, "::ffff:a9fe:1", false, true, true, false, "169.254.0.1"},
    {"64:ff9b:1::1", true, "64:ff9b:1::1", false, true, false, false, "64:ff9b:1::1"},
    {"2002::1", true, "2002::1", false, true, false, false, "2002::1"},
    {"3fff::1", true, "3fff::1", false, true, false, false, "3fff::1"},
    {"2001:1::1", true, "2001:1::1", false, false, false, true, "2001:1::1"},
    {"2001:3::1", true, "2001:3::1", false, false, false, true, "2001:3::1"},
    {"2001:20::1", true, "2001:20::1", false, false, false, true, "2001:20::1"},
    {"2001::1", true, "2001::1", false, true, false, false, "2001::1"},
    {"::127.0.0.1", true, "::7f00:1", false, false, false, true, "::7f00:1"},
    {"1:2:3:4:5:6:7:8", true, "1:2:3:4:5:6:7:8", false, false, false, true, "1:2:3:4:5:6:7:8"},
    {"1::8", true, "1::8", false, false, false, true, "1::8"},
    {"1:0:0:2:0:0:0:3", true, "1:0:0:2::3", false, false, false, true, "1:0:0:2::3"},
    {"0:0:0:0:0:0:0:0", true, "::", false, true, false, false, "::"},
    {"1:0:0:0:0:0:0:0", true, "1::", false, false, false, true, "1::"},
    {"0:0:1:0:0:0:0:0", true, "0:0:1::", false, false, false, true, "0:0:1::"},
    {"FE80::ABCD", true, "fe80::abcd", false, true, true, false, "fe80::abcd"},
    {"::ffff:1.2.3.4", true, "::ffff:102:304", false, false, false, true, "1.2.3.4"},
    {"1:2:3:4:5:6:1.2.3.4", true, "1:2:3:4:5:6:102:304", false, false, false, true,
     "1:2:3:4:5:6:102:304"},
    {"", false, "", false, false, false, false, ""},
    {"localhost", false, "", false, false, false, false, ""},
    {"01.2.3.4", false, "", false, false, false, false, ""},
    {"1.2.3", false, "", false, false, false, false, ""},
    {"1.2.3.4.5", false, "", false, false, false, false, ""},
    {"256.1.1.1", false, "", false, false, false, false, ""},
    {"1.2.3.4 ", false, "", false, false, false, false, ""},
    {" 1.2.3.4", false, "", false, false, false, false, ""},
    {"1.2.3.-4", false, "", false, false, false, false, ""},
    {"::1::", false, "", false, false, false, false, ""},
    {"1:::2", false, "", false, false, false, false, ""},
    {":1::2", false, "", false, false, false, false, ""},
    {"1::2:", false, "", false, false, false, false, ""},
    {"12345::", false, "", false, false, false, false, ""},
    {"1:2:3:4:5:6:7:8:9", false, "", false, false, false, false, ""},
    {"1:2:3:4:5:6:7::", true, "1:2:3:4:5:6:7:0", false, false, false, true, "1:2:3:4:5:6:7:0"},
    {"::1:2:3:4:5:6:7", true, "0:1:2:3:4:5:6:7", false, false, false, true, "0:1:2:3:4:5:6:7"},
    {"1:2:3:4:5:6:7:8::", false, "", false, false, false, false, ""},
    {"fe80::1%", false, "", false, false, false, false, ""},
    {"fe80::1%a%b", false, "", false, false, false, false, ""},
    {"1.2.3.4%1", false, "", false, false, false, false, ""},
    {"g::1", false, "", false, false, false, false, ""},
    {"::1/128", false, "", false, false, false, false, ""},
    {"\xEF\xBC\x91.2.3.4", false, "", false, false, false, false, ""},
    {"1::2::3", false, "", false, false, false, false, ""},
    {":::", false, "", false, false, false, false, ""},
};

} // namespace

TEST_CASE("addresses parse and classify as Python's ipaddress does") {
    for (const auto& c : kCases) {
        INFO(c.text);
        const auto address = parse_address(c.text);
        REQUIRE(address.has_value() == c.valid);
        if (!address) {
            continue;
        }
        CHECK(address->to_string() == c.str);
        CHECK(is_loopback(*address) == c.loopback);
        CHECK(is_private(*address) == c.private_);
        CHECK(is_link_local(*address) == c.link_local);
        CHECK(is_global(*address) == c.global);
        CHECK(unmap_v4(*address).to_string() == c.unmapped);
    }
}

TEST_CASE("an address keeps its scope and versions compare apart") {
    const auto scoped = parse_address("fe80::1%3");
    REQUIRE(scoped);
    CHECK(scoped->version == 6);
    CHECK(scoped->scope_id == "3");
    CHECK(*scoped != *parse_address("fe80::1"));
    CHECK(*parse_address("FE80::1") == *parse_address("fe80:0:0::1"));
    CHECK(*parse_address("::ffff:127.0.0.1") != *parse_address("127.0.0.1"));
    CHECK(unmap_v4(*parse_address("::ffff:127.0.0.1")) == *parse_address("127.0.0.1"));
}

TEST_CASE("strip_scope drops what follows the first percent sign") {
    CHECK(strip_scope("fe80::1%3") == "fe80::1");
    CHECK(strip_scope("fe80::1%eth0%x") == "fe80::1");
    CHECK(strip_scope("127.0.0.1") == "127.0.0.1");
    CHECK(strip_scope("%") == "");
}

TEST_CASE("can_bind is true for loopback and false for an address no interface has", "[platform]") {
    CHECK(can_bind(*parse_address("127.0.0.1")));
    CHECK_FALSE(can_bind(*parse_address("192.0.2.1"))); // TEST-NET-1, never assigned
}

TEST_CASE("resolve_host gives a loopback address for localhost", "[platform]") {
    const auto found = resolve_host("localhost", CancelToken());
    REQUIRE(found.has_value());
    REQUIRE_FALSE(found->empty());
    CHECK(std::ranges::any_of(*found, [](const IpAddress& a) { return is_loopback(a); }));
    for (const auto& address : *found) {
        CHECK(address.scope_id.empty());
    }
}

TEST_CASE("resolve_host answers a numeric address without the network", "[platform]") {
    const auto found = resolve_host("127.0.0.1", CancelToken());
    REQUIRE(found.has_value());
    REQUIRE(found->size() == 1);
    CHECK(found->front().to_string() == "127.0.0.1");
}

TEST_CASE("resolve_host returns at once for a cancelled token") {
    CancelSource source;
    source.cancel();
    const auto found = resolve_host("localhost", source.token());
    REQUIRE_FALSE(found.has_value());
    CHECK(found.error() == "cancelled");
}