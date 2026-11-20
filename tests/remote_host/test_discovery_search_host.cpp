// test_remote_pairing_ux.py::test_a_sharing_host_answers_a_search_with_where_to_connect: the
// client's search (remote::search) against a sharing SpeechHost over a FakeDiscoveryBus.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "host_rig.h"
#include "net/reactor.h"
#include "remote/client.h"
#include "remote/discovery_search.h"
#include "remote/testing/fake_discovery_bus.h"

using namespace fv::remote_host::rig;
using namespace std::chrono_literals;

TEST_CASE("test_a_sharing_host_answers_a_search_with_where_to_connect") {
    fv::remote::testing::FakeDiscoveryBus bus;
    HostRig rig(
        [&](fv::remote_host::SpeechHostDeps& deps) { deps.discovery = bus.factory("127.0.0.1"); });
    rig.host->stop();
    REQUIRE(rig.host->start(0, "127.0.0.1", 0).has_value());
    const auto udp = rig.host->discovery_port();
    REQUIRE(udp.has_value());

    fv::net::Reactor reactor;
    fv::remote::ClientEnv env{.reactor = reactor, .discovery = bus.factory("192.168.1.30")};
    const std::vector<std::pair<std::string, int>> targets{{std::string("127.0.0.1"), *udp}};
    const auto found = fv::remote::search(env, 1200ms, targets);
    REQUIRE(found.size() == 1);
    const auto& host = found.front();
    CHECK(host.host_name == "devbox");
    CHECK(host.port == *rig.host->port());
    CHECK(host.fingerprint == test_identity().fingerprint);
    CHECK(host.engine.at("label") == "Fake Parakeet");
    CHECK(host.approval);
    CHECK(host.compatible());

    rig.host->stop();
    CHECK(fv::remote::search(env, 300ms, targets).empty());
}
