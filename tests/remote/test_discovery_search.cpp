// remote/discovery_search.h against services/remote_asr/discovery.py's searching side (search,
// find_host, sweep) over the in-memory discovery bus and loopback sockets, and
// remote/discovery_transport.h's udp_discovery() by a unicast round trip on 127.0.0.1 (a
// broadcast on real network cards would be [interactive]).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "client_test_support.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "remote/discovery_codec.h"
#include "remote/discovery_search.h"
#include "remote/discovery_transport.h"
#include "remote/wire.h"

using namespace fv;
using namespace fv::remote;
using namespace fv::remote::test;
using namespace std::chrono_literals;

namespace {

std::string fingerprint_of(std::string_view pair) {
    std::string fp;
    for (int i = 0; i < 32; ++i) {
        fp += pair;
    }
    return fp;
}

Json info(std::string name, int port, std::string fingerprint) {
    return Json{{"name", std::move(name)},
                {"port", port},
                {"fingerprint", std::move(fingerprint)},
                {"approval", true},
                {"engine", Json{{"label", "Parakeet"}, {"available", true}}}};
}

// A TCP listener on 127.0.0.1 that accepts and answers nothing: what a sweep knocks on.
struct Listener {
    explicit Listener(net::Reactor& reactor) : server(net::HttpServer::create(reactor)) {
        auto endpoint = server->listen(net::Listen{.address = "127.0.0.1", .port = 0});
        REQUIRE(endpoint.has_value());
        port = endpoint->port;
    }
    std::unique_ptr<net::HttpServer> server;
    std::uint16_t port = 0;
};

} // namespace

// ---- tests/test_remote_pairing_ux.py ----

TEST_CASE("find_host stops at the certificate it wants", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_find_host_stops_at_the_certificate_it_wants
    ClientRig rig;
    const std::string wanted = fingerprint_of("AB");
    BusResponder other(rig.bus, "192.168.1.30", info("nas", 47821, fingerprint_of("EF")));
    BusResponder host(rig.bus, "192.168.1.20", info("devbox", 5000, wanted));
    const auto started = std::chrono::steady_clock::now();
    const auto found = find_host(rig.env, fingerprint_of("ab"), 3s);
    REQUIRE(found.has_value());
    CHECK(found->port == 5000);
    CHECK(found->address == "192.168.1.20");
    CHECK(found->fingerprint == wanted);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    CHECK(!find_host(rig.env, fingerprint_of("CD"), 500ms).has_value());
    CHECK(!find_host(rig.env, "", 500ms).has_value());
}

TEST_CASE("sweep knocks on each address and keeps those that answer", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::
    // test_sweep_knocks_on_each_address_and_keeps_those_that_answer
    ClientRig rig;
    std::uint16_t port = 0;
    {
        Listener listener(rig.reactor);
        port = listener.port;
        CHECK(sweep(rig.env, port, std::vector<std::string>{"127.0.0.1"}, 1s) ==
              std::vector<std::string>{"127.0.0.1"});
    }
    CHECK(sweep(rig.env, port, std::vector<std::string>{"127.0.0.1"}, 1s).empty());
    CHECK(sweep(rig.env, port, std::vector<std::string>{}).empty());
}

// ---- the rest of the search ----

TEST_CASE("a search queries each broadcast address three times and keeps one answer per "
          "certificate") {
    ClientRig rig;
    rig.env.lan_cards = [] {
        return std::vector<LanCard>{{"192.168.1.5", 24, true, true}, {"10.8.0.2", 24, false, true}};
    };
    BusResponder first(rig.bus, "192.168.1.20", info("devbox", 47821, fingerprint_of("AB")));
    // The same computer answering from another address: one entry, the first answer's.
    BusResponder again(rig.bus, "192.168.1.21", info("devbox", 47821, fingerprint_of("ab")));
    BusResponder nas(rig.bus, "192.168.1.30", info("nas", 47821, fingerprint_of("CD")));
    const auto hosts = search(rig.env, 1200ms);
    std::set<std::string> fingerprints;
    for (const auto& host : hosts) {
        fingerprints.insert(host.fingerprint);
    }
    CHECK(hosts.size() == 2);
    CHECK(fingerprints == std::set<std::string>{fingerprint_of("AB"), fingerprint_of("CD")});

    std::vector<std::string> targets;
    std::set<std::vector<std::byte>> queries;
    for (const auto& sent : rig.bus.sent()) {
        if (sent.from == "10.0.0.2") {
            targets.push_back(sent.to + ":" + std::to_string(sent.port));
            queries.insert(sent.bytes);
            CHECK(sent.bytes.size() == QUERY_BYTES);
        }
    }
    // The LAN card's broadcast address, then everyone's, at 0, 0.3 and 0.7 s; the VPN card
    // gets none.
    CHECK(targets == std::vector<std::string>{"192.168.1.255:47821", "255.255.255.255:47821",
                                              "192.168.1.255:47821", "255.255.255.255:47821",
                                              "192.168.1.255:47821", "255.255.255.255:47821"});
    CHECK(queries.size() == 1);
}

TEST_CASE("a search with targets asks only them, and a port of 0 not at all") {
    ClientRig rig;
    BusResponder host(rig.bus, "192.168.1.20", info("devbox", 47821, fingerprint_of("AB")));
    const auto none =
        search(rig.env, 400ms, std::vector<std::pair<std::string, int>>{{"192.168.1.20", 0}});
    CHECK(none.empty());
    CHECK(rig.bus.sent().empty());
    const auto found = search(
        rig.env, 400ms, std::vector<std::pair<std::string, int>>{{"192.168.1.20", DISCOVERY_PORT}});
    REQUIRE(found.size() == 1);
    CHECK(found[0].host_name == "devbox");
    CHECK(found[0].where() == "192.168.1.20:47821");
}

TEST_CASE("a search ignores answers to another query and strays") {
    ClientRig rig;
    // A computer answering every datagram with an answer to someone else's query.
    auto liar = rig.bus.transport("192.168.1.40");
    REQUIRE(liar->bind("0.0.0.0", DISCOVERY_PORT).has_value());
    std::atomic<bool> stop{false};
    std::thread answering([&] {
        const std::vector<std::uint8_t> other(DISCOVERY_NONCE_BYTES, 7);
        while (!stop) {
            if (const auto datagram = liar->receive(20ms)) {
                liar->send_to(encode_reply(other, info("liar", 47821, fingerprint_of("EE"))),
                              datagram->sender, datagram->port);
                const std::vector<std::byte> junk(8, std::byte{1});
                liar->send_to(junk, datagram->sender, datagram->port);
            }
        }
    });
    CHECK(search(rig.env, 500ms).empty());
    stop = true;
    liar->close();
    answering.join();
}

TEST_CASE("a search without a discovery socket finds nothing and sends nothing") {
    ClientRig rig;
    rig.env.discovery = {};
    CHECK(search(rig.env, 200ms).empty());
    CHECK(!find_host(rig.env, fingerprint_of("AB"), 200ms).has_value());
}

TEST_CASE("a sweep without addresses knocks on this network's /24, every one at once") {
    ClientRig rig;
    rig.env.lan_cards = [] {
        return std::vector<LanCard>{{"192.168.7.9", 24, true, true}};
    };
    std::mutex mutex;
    std::set<std::string> knocked;
    std::set<std::uint16_t> ports;
    std::set<std::chrono::milliseconds::rep> timeouts;
    rig.env.knock = [&](std::string_view address, std::uint16_t port,
                        std::chrono::milliseconds timeout) {
        std::lock_guard lock(mutex);
        knocked.insert(std::string(address));
        ports.insert(port);
        timeouts.insert(timeout.count());
        return address == "192.168.7.12" || address == "192.168.7.200";
    };
    CHECK(sweep(rig.env) == std::vector<std::string>{"192.168.7.12", "192.168.7.200"});
    CHECK(ports == std::set<std::uint16_t>{47821});
    CHECK(timeouts == std::set<std::chrono::milliseconds::rep>{400});
    CHECK(knocked.size() == 253);
    CHECK(!knocked.contains("192.168.7.9"));
}

// ---- udp_discovery ----

TEST_CASE("udp_discovery carries a query and its answer over loopback", "[platform]") {
    const auto factory = udp_discovery();
    auto responder = factory(false);
    const auto bound = responder->bind("127.0.0.1", 0);
    REQUIRE(bound.has_value());
    REQUIRE(*bound != 0);
    auto searcher = factory(true);

    const std::vector<std::uint8_t> nonce(DISCOVERY_NONCE_BYTES, 3);
    REQUIRE(searcher->send_to(make_query(nonce), "127.0.0.1", *bound));
    const auto query = responder->receive(2s);
    REQUIRE(query.has_value());
    CHECK(query->sender == "127.0.0.1");
    CHECK(query->port != 0);
    const auto asked = parse_query(query->bytes);
    REQUIRE(asked.has_value());
    CHECK(*asked == nonce);
    REQUIRE(responder->send_to(encode_reply(*asked, info("devbox", 5000, fingerprint_of("AB"))),
                               query->sender, query->port));
    const auto answer = searcher->receive(2s);
    REQUIRE(answer.has_value());
    const auto host = parse_reply(answer->bytes, nonce, answer->sender);
    REQUIRE(host.has_value());
    CHECK(host->host_name == "devbox");
    CHECK(host->port == 5000);

    // Nothing more: a receive times out, and close() ends one waiting on another thread.
    CHECK(!searcher->receive(50ms).has_value());
    std::atomic<bool> received_after_close{true};
    std::thread waiting([&] { received_after_close = responder->receive(10s).has_value(); });
    std::this_thread::sleep_for(100ms);
    const auto started = std::chrono::steady_clock::now();
    responder->close();
    waiting.join();
    CHECK(!received_after_close);
    CHECK(std::chrono::steady_clock::now() - started < 5s);
    searcher->close();
    CHECK(!searcher->send_to(make_query(nonce), "127.0.0.1", *bound));
}

TEST_CASE("a udp_discovery socket that cannot bind says why", "[platform]") {
    const auto factory = udp_discovery();
    auto first = factory(false);
    const auto bound = first->bind("127.0.0.1", 0);
    REQUIRE(bound.has_value());
    auto second = factory(false);
    const auto refused = second->bind("127.0.0.1", *bound);
    REQUIRE(!refused.has_value());
    CHECK(!refused.error().empty());
    // Never bound: nothing to send from, nothing to receive.
    CHECK(!second->send_to(make_query(std::vector<std::uint8_t>(16, 1)), "127.0.0.1", *bound));
    CHECK(!second->receive(10ms).has_value());
    first->close();
    second->close();
}

TEST_CASE("a search over this computer's network cards finds a responder on them",
          "[interactive]") {
    // Binds every interface and broadcasts on the real network cards, which can bring up a
    // firewall prompt and reaches the computers beside this one: the owner runs it.
    net::Reactor reactor;
    const auto factory = udp_discovery();
    auto responder = factory(false);
    const auto bound = responder->bind("0.0.0.0", 0);
    REQUIRE(bound.has_value());
    const std::string fingerprint = fingerprint_of("AB");
    std::atomic<bool> stop{false};
    std::thread answering([&] {
        while (!stop) {
            if (const auto datagram = responder->receive(50ms)) {
                if (const auto nonce = parse_query(datagram->bytes)) {
                    (void)responder->send_to(encode_reply(*nonce, info("me", 5000, fingerprint)),
                                             datagram->sender, datagram->port);
                }
            }
        }
    });
    ClientEnv env{.reactor = reactor, .discovery = factory, .lan_cards = system_lan_cards};
    std::vector<std::pair<std::string, int>> targets;
    for (const auto& address : broadcast_targets(system_lan_cards())) {
        targets.emplace_back(address, *bound);
    }
    const auto found = search(env, 1200ms, targets);
    stop = true;
    responder->close();
    answering.join();
    CHECK(std::ranges::any_of(
        found, [&](const LanHost& host) { return host.fingerprint == fingerprint; }));
}
