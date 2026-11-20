// net/tailscale.h replayed against services/remote_asr/tailscale.py
// (tests/golden/services.remote_asr.tailscale.json).

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "golden/golden.h"
#include "net/tailscale.h"
#include "pyport/errors.h"

using namespace fv;
using namespace fv::net;

namespace {

constexpr std::string_view kModule = "services.remote_asr.tailscale";

// The wrapper's record: dataclasses.asdict(status) plus running, and is_desktop per peer.
Json as_record(const TailscaleStatus& status) {
    Json peers = Json::array();
    for (const auto& peer : status.peers) {
        peers.push_back({{"name", peer.name},
                         {"dns_name", peer.dns_name},
                         {"address", peer.address},
                         {"os", peer.os},
                         {"online", peer.online},
                         {"owner", peer.owner},
                         {"tagged", peer.tagged},
                         {"mine", peer.mine},
                         {"is_desktop", peer.is_desktop()}});
    }
    return {{"state", status.state},      {"detail", status.detail},
            {"name", status.name},        {"dns_name", status.dns_name},
            {"address", status.address},  {"owner", status.owner},
            {"tailnet", status.tailnet},  {"peers", peers},
            {"running", status.running()}};
}

} // namespace

TEST_CASE("is_tailscale_address matches Python", "[golden]") {
    golden::for_each(kModule, "is_tailscale_address", [](const golden::Case& c) {
        CHECK(is_tailscale_address(c.arg<std::string>(0)) == c.result<bool>());
    });
}

TEST_CASE("parse_status matches Python", "[golden]") {
    golden::for_each(kModule, "parse_status", [](const golden::Case& c) {
        const Json data = c.arg<Json>(0);
        if (c.raises()) {
            try {
                (void)parse_status(data);
                FAIL("expected " << c.raises_type());
            } catch (const py::PyError& error) {
                CHECK(error.type_name() == c.raises_type());
            }
            return;
        }
        const Json actual = as_record(parse_status(data));
        const Json expected = c.result<Json>();
        INFO("expected " << expected.dump() << "\nactual   " << actual.dump());
        CHECK(golden::equal(expected, actual));
    });
}

TEST_CASE("an address object is classified like its text") {
    const auto mapped = parse_address("::ffff:100.64.0.2");
    REQUIRE(mapped);
    CHECK(is_tailscale_address(*mapped));
    CHECK_FALSE(is_tailscale_address(*parse_address("192.168.1.20")));
    CHECK(is_tailscale_address(*parse_address("fd7a:115c:a1e0::1%7")));
}

TEST_CASE("parse_status refuses a document that is not an object") {
    CHECK_THROWS_AS(parse_status(Json::array()), std::invalid_argument);
}
