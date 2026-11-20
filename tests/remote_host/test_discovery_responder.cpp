// The host's discovery answers (services/remote_asr/discovery.py DiscoveryResponder;
// tests/test_remote_pairing_ux.py), over the in-memory UDP network.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "host_rig.h"
#include "remote/discovery_codec.h"
#include "remote/testing/fake_discovery_bus.h"
#include "remote_host/discovery_responder.h"

using fv::Json;
using fv::remote_host::DiscoveryResponder;
using namespace fv::remote_host::rig;
using namespace std::chrono_literals;

namespace {

std::vector<std::byte> query(std::uint8_t fill = 'n') {
    std::array<std::uint8_t, fv::remote::DISCOVERY_NONCE_BYTES> nonce{};
    nonce.fill(fill);
    return fv::remote::make_query(nonce);
}

Json devbox_info() {
    return Json{{"name", "devbox"}, {"port", 1}, {"fingerprint", std::string(64, 'A')}};
}

} // namespace

TEST_CASE("test_the_responder_limits_how_often_it_answers_one_address") {
    DiscoveryResponder responder(devbox_info);
    const auto q = query();
    int answered = 0;
    for (int i = 0; i < 30; ++i) {
        answered += responder.answer(q, "192.168.1.30") ? 1 : 0;
    }
    CHECK(answered == fv::remote::MAX_ANSWERS_PER_SENDER);
    CHECK(responder.answer(q, "192.168.1.31").has_value()); // someone else still gets one
    CHECK_FALSE(responder.answer(q, "8.8.8.8").has_value());
}

TEST_CASE("the responder answers again once its window has passed, and 200 in all") {
    fv::FakeClock clock;
    DiscoveryResponder responder(devbox_info, clock);
    const auto q = query();
    int answered = 0;
    for (int sender = 0; sender < 15; ++sender) {
        const std::string address = "192.168.1." + std::to_string(sender + 10);
        for (int i = 0; i < 20; ++i) {
            answered += responder.answer(q, address) ? 1 : 0;
        }
    }
    CHECK(answered == fv::remote::MAX_ANSWERS);
    CHECK_FALSE(responder.answer(q, "192.168.2.1").has_value());
    clock.advance(10s);
    CHECK_FALSE(responder.answer(q, "192.168.2.1").has_value());
    clock.advance(1ms);
    CHECK(responder.answer(q, "192.168.2.1").has_value());
}

TEST_CASE("the responder stays quiet for anything but a query") {
    DiscoveryResponder responder(devbox_info);
    auto short_query = query();
    short_query.resize(100);
    CHECK_FALSE(responder.answer(short_query, "192.168.1.30").has_value());
    CHECK_FALSE(responder.answer(std::vector<std::byte>(512, std::byte{0}), "192.168.1.30"));
    CHECK_FALSE(responder.answer(query(), "unknown").has_value());
}

TEST_CASE("a host answers discovery queries on the network with who it is") {
    fv::remote::testing::FakeDiscoveryBus bus;
    HostRig rig(
        [&](fv::remote_host::SpeechHostDeps& deps) { deps.discovery = bus.factory("127.0.0.1"); });
    rig.host->stop();
    REQUIRE(rig.host->start(0, "127.0.0.1", 0).has_value());
    const auto udp = rig.host->discovery_port();
    REQUIRE(udp.has_value());
    CHECK(rig.host->discovery_error().empty());

    auto searcher = bus.transport("192.168.1.30");
    std::array<std::uint8_t, fv::remote::DISCOVERY_NONCE_BYTES> nonce{};
    nonce.fill('q');
    REQUIRE(searcher->send_to(fv::remote::make_query(nonce), "127.0.0.1", *udp));
    const auto answer = searcher->receive(5s);
    REQUIRE(answer.has_value());
    const auto host = fv::remote::parse_reply(answer->bytes, nonce, answer->sender);
    REQUIRE(host.has_value());
    CHECK(host->host_name == "devbox");
    CHECK(host->port == *rig.host->port());
    CHECK(host->fingerprint == test_identity().fingerprint);
    CHECK(host->approval);
    CHECK(host->engine == Json{{"label", "Fake Parakeet"},
                               {"device", "cuda"},
                               {"available", true},
                               {"family", "parakeet"}});
    rig.host->stop();
    CHECK_FALSE(rig.host->discovery_port().has_value());
    for (const auto& bound : bus.bound()) {
        CHECK(bound != "127.0.0.1:" + std::to_string(*udp));
    }
}

TEST_CASE("test_a_taken_discovery_port_leaves_sharing_running") {
    fv::remote::testing::FakeDiscoveryBus bus;
    bus.fail_bind("127.0.0.1", 47821,
                  "Only one usage of each socket address is normally permitted");
    HostRig rig(
        [&](fv::remote_host::SpeechHostDeps& deps) { deps.discovery = bus.factory("127.0.0.1"); });
    rig.host->stop();
    const auto started = rig.host->start(0, "127.0.0.1", fv::remote::DISCOVERY_PORT);
    REQUIRE(started.has_value());
    rig.port = *started;
    CHECK(rig.host->running());
    CHECK(rig.host->discovery_error() ==
          "UDP port 47821 is in use (Only one usage of each socket address is normally permitted)");
    CHECK_FALSE(rig.host->discovery_port().has_value());
    // Sharing itself still works.
    CHECK(!rig.pair().token.empty());
}
