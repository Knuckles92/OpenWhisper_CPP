// remote/discovery_codec.h against services/remote_asr/discovery.py: the ported tests of
// tests/test_remote_pairing_ux.py, the golden vectors
// (tests/golden/services.remote_asr.discovery.json), and the in-memory UDP network the
// search and responder tests of later packages run on.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "golden/golden.h"
#include "net/address.h"
#include "pyport/errors.h"
#include "remote/discovery_codec.h"
#include "remote/discovery_transport.h"
#include "remote/testing/fake_discovery_bus.h"
#include "remote_test_support.h"

using namespace fv;
using namespace fv::remote;
using fv::remote::test::bytes_from_hex;
using fv::remote::test::bytes_of;
using fv::remote::test::from_hex;
using fv::remote::test::hex_of;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kModule = "services.remote_asr.discovery";

std::vector<std::uint8_t> nonce_of(char ch) {
    return std::vector<std::uint8_t>(DISCOVERY_NONCE_BYTES, static_cast<std::uint8_t>(ch));
}

std::string fingerprint_ab() {
    std::string fp;
    for (int i = 0; i < 32; ++i) {
        fp += "AB";
    }
    return fp;
}

Json host_record(const LanHost& host) {
    return {{"address", host.address},        {"port", host.port},
            {"host_name", host.host_name},    {"fingerprint", host.fingerprint},
            {"engine", host.engine},          {"protocol", host.protocol},
            {"approval", host.approval},      {"where", host.where()},
            {"compatible", host.compatible()}};
}

std::vector<LanCard> cards_from(const Json& entries) {
    std::vector<LanCard> cards;
    for (const auto& entry : entries) {
        LanCard card;
        card.address = entry[0].get<std::string>();
        card.lan = entry[1] == "lan";
        card.prefix = entry[2].get<int>();
        const auto parsed = net::parse_address(card.address);
        card.is_private = parsed && net::is_private(*parsed);
        cards.push_back(card);
    }
    return cards;
}

} // namespace

// ---- tests/test_remote_pairing_ux.py ----

TEST_CASE("queries are padded and answers never outgrow them", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_queries_are_padded_and_answers_never_outgrow_them
    const auto nonce = nonce_of('n');
    const auto query = make_query(nonce);
    CHECK(query.size() == QUERY_BYTES);
    CHECK(parse_query(query) == nonce);
    CHECK_FALSE(parse_query(std::span(query).first(100))); // short: no answer to amplify
    CHECK_FALSE(parse_query(std::vector<std::byte>(QUERY_BYTES, std::byte{'x'})));

    std::string long_name;
    for (int i = 0; i < 60; ++i) {
        long_name += "\xE6\xBC\xA2"; // three bytes each in UTF-8
    }
    const Json info = {{"name", long_name},
                       {"port", 47821},
                       {"fingerprint", fingerprint_ab()},
                       {"approval", true},
                       {"engine",
                        {{"label", std::string(300, 'L')},
                         {"device", "cuda"},
                         {"available", true},
                         {"family", "whisper"}}}};
    const auto reply = encode_reply(nonce, info);
    CHECK(!reply.empty());
    CHECK(reply.size() <= QUERY_BYTES);
    const auto host = parse_reply(reply, nonce, "192.168.1.20");
    REQUIRE(host);
    CHECK(host->fingerprint == fingerprint_ab());
    CHECK(host->port == 47821);
    CHECK(host->approval);
    CHECK(host->where() == "192.168.1.20:47821");
    CHECK(host->compatible());
}

TEST_CASE("answers to another query or with a bad certificate are ignored",
          "[test_remote_pairing_ux]") {
    // from
    // tests/test_remote_pairing_ux.py::test_answers_to_another_query_or_with_a_bad_certificate_are_ignored
    const auto nonce = nonce_of('n');
    const Json info = {{"name", "devbox"}, {"port", 47821}, {"fingerprint", fingerprint_ab()}};
    CHECK_FALSE(parse_reply(encode_reply(nonce_of('o'), info), nonce, "x"));
    Json bad = info;
    bad["fingerprint"] = "not hex";
    CHECK_FALSE(parse_reply(encode_reply(nonce, bad), nonce, "x"));
    Json no_port = info;
    no_port["port"] = 0;
    CHECK_FALSE(parse_reply(encode_reply(nonce, no_port), nonce, "x"));
    CHECK_FALSE(parse_reply(bytes_of("garbage"), nonce, "x"));
}

TEST_CASE("only this network gets answers", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_only_this_network_gets_answers
    const auto [address, allowed] = GENERATE(table<std::string, bool>({
        {"192.168.1.20", true},
        {"10.0.0.5", true},
        {"172.16.0.1", true},
        {"127.0.0.1", true},
        {"169.254.1.1", true},
        {"100.64.0.5", true},
        {"8.8.8.8", false},
        {"224.0.0.1", false},
        {"nonsense", false},
    }));
    CHECK(allowed_sender(address) == allowed);
}

// ---- golden vectors ----

TEST_CASE("discovery constants match Python", "[golden]") {
    golden::for_each(kModule, "constants", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        CHECK(expected["DISCOVERY_PORT"] == DISCOVERY_PORT);
        CHECK(expected["NONCE_BYTES"] == DISCOVERY_NONCE_BYTES);
        CHECK(expected["QUERY_BYTES"] == QUERY_BYTES);
        CHECK(expected["_DATAGRAM_MAX_BYTES"] == DATAGRAM_MAX_BYTES);
        CHECK(expected["SEARCH_S"] == SEARCH_S);
        CHECK((expected["_SEND_AT"] == Json(SEND_AT_S)));
        CHECK(expected["_MAX_ANSWERS_PER_SENDER"] == MAX_ANSWERS_PER_SENDER);
        CHECK(expected["_MAX_ANSWERS"] == MAX_ANSWERS);
        CHECK(expected["_RATE_WINDOW_S"] == RATE_WINDOW_S);
        CHECK(expected["SWEEP_CONNECT_S"] == SWEEP_CONNECT_S);
        CHECK(expected["_SWEEP_WORKERS"] == SWEEP_WORKERS);
        CHECK(expected["_SWEEP_NETWORKS"] == SWEEP_NETWORKS);
        CHECK(expected["_MAX_NAME"] == MAX_NAME);
        CHECK(expected["QUERY_MAGIC"] == hex_of(bytes_of(QUERY_MAGIC)));
        CHECK(expected["REPLY_MAGIC"] == hex_of(bytes_of(REPLY_MAGIC)));
    });
}

TEST_CASE("queries match Python", "[golden]") {
    golden::for_each(kModule, "make_query", [](const golden::Case& c) {
        CHECK(hex_of(make_query(from_hex(c.arg<std::string>(0)))) == c.result<std::string>());
    });
    golden::for_each(kModule, "parse_query", [](const golden::Case& c) {
        const auto nonce = parse_query(bytes_from_hex(c.arg<std::string>(0)));
        const Json actual = nonce ? Json(hex_of(std::as_bytes(std::span(*nonce)))) : Json(nullptr);
        CHECK((actual == c.result<Json>()));
    });
}

TEST_CASE("encode_reply matches Python byte for byte", "[golden]") {
    golden::for_each(kModule, "encode_reply", [](const golden::Case& c) {
        const auto nonce = from_hex(c.arg<std::string>(0));
        if (c.raises()) {
            try {
                (void)encode_reply(nonce, c.arg<Json>(1));
                FAIL("expected " << c.raises_type());
            } catch (const py::PyError& error) {
                CHECK(error.type_name() == c.raises_type());
            }
            return;
        }
        CHECK(hex_of(encode_reply(nonce, c.arg<Json>(1))) == c.result<std::string>());
    });
}

TEST_CASE("parse_reply matches Python", "[golden]") {
    golden::for_each(kModule, "parse_reply", [](const golden::Case& c) {
        const auto data = bytes_from_hex(c.arg<std::string>(0));
        const auto nonce = from_hex(c.arg<std::string>(1));
        const std::string address = c.arg<std::string>(2);
        if (c.raises()) {
            try {
                (void)parse_reply(data, nonce, address);
                FAIL("expected " << c.raises_type());
            } catch (const py::PyError& error) {
                CHECK(error.type_name() == c.raises_type());
            }
            return;
        }
        const auto host = parse_reply(data, nonce, address);
        const Json actual = host ? host_record(*host) : Json(nullptr);
        INFO(actual.dump());
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("allowed_sender matches Python", "[golden]") {
    golden::for_each(kModule, "allowed_sender", [](const golden::Case& c) {
        CHECK(allowed_sender(c.arg<std::string>(0)) == c.result<bool>());
    });
}

TEST_CASE("broadcast targets and sweep addresses match Python", "[golden]") {
    golden::for_each(kModule, "broadcast_targets", [](const golden::Case& c) {
        const auto cards = cards_from(c.arg<Json>(0));
        CHECK(broadcast_targets(cards) == c.result<std::vector<std::string>>());
    });
    golden::for_each(kModule, "sweep_addresses", [](const golden::Case& c) {
        const auto cards = cards_from(c.arg<Json>(0));
        CHECK(sweep_addresses(cards) == c.result<std::vector<std::string>>());
    });
}

// ---- the fake network ----

TEST_CASE("a datagram reaches the socket bound to its port on that computer", "[discovery_bus]") {
    testing::FakeDiscoveryBus bus;
    auto host = bus.transport("192.168.1.20");
    REQUIRE(host->bind("0.0.0.0", DISCOVERY_PORT) == DISCOVERY_PORT);
    auto client = bus.transport("192.168.1.30");
    const auto query = make_query(nonce_of('n'));
    CHECK(client->send_to(query, "192.168.1.20", DISCOVERY_PORT));
    const auto received = host->receive(100ms);
    REQUIRE(received);
    CHECK(received->bytes == query);
    CHECK(received->sender == "192.168.1.30");
    CHECK(received->port != 0);

    // The answer goes back to where the query came from.
    CHECK(host->send_to(bytes_of("answer"), received->sender, received->port));
    const auto answer = client->receive(100ms);
    REQUIRE(answer);
    CHECK(answer->bytes == bytes_of("answer"));
    CHECK(answer->sender == "192.168.1.20");
    CHECK(answer->port == DISCOVERY_PORT);
    CHECK_FALSE(host->receive(10ms));
}

TEST_CASE("broadcasts need SO_BROADCAST and reach every computer listed", "[discovery_bus]") {
    testing::FakeDiscoveryBus bus;
    bus.add_broadcast("192.168.1.255", {"192.168.1.20", "192.168.1.21"});
    auto first = bus.transport("192.168.1.20");
    auto second = bus.transport("192.168.1.21");
    auto elsewhere = bus.transport("10.0.0.9");
    REQUIRE(first->bind("", DISCOVERY_PORT));
    REQUIRE(second->bind("", DISCOVERY_PORT));
    REQUIRE(elsewhere->bind("", DISCOVERY_PORT));

    auto plain = bus.transport("192.168.1.30");
    CHECK_FALSE(plain->send_to(bytes_of("q"), "192.168.1.255", DISCOVERY_PORT));
    auto searcher = bus.transport("192.168.1.30", true);
    CHECK(searcher->send_to(bytes_of("q"), "192.168.1.255", DISCOVERY_PORT));
    CHECK(first->receive(50ms));
    CHECK(second->receive(50ms));
    CHECK_FALSE(elsewhere->receive(10ms));

    CHECK(searcher->send_to(bytes_of("q"), testing::FakeDiscoveryBus::ALL_HOSTS, DISCOVERY_PORT));
    CHECK(first->receive(50ms));
    CHECK(second->receive(50ms));
    CHECK(elsewhere->receive(50ms));
}

TEST_CASE("a port in use, an injected bind failure and a closed socket", "[discovery_bus]") {
    testing::FakeDiscoveryBus bus;
    auto first = bus.transport("127.0.0.1");
    REQUIRE(first->bind("127.0.0.1", DISCOVERY_PORT));
    auto second = bus.transport("127.0.0.1");
    CHECK_FALSE(second->bind("127.0.0.1", DISCOVERY_PORT));
    const auto ephemeral = second->bind("127.0.0.1", 0);
    REQUIRE(ephemeral);
    CHECK(*ephemeral != 0);

    bus.fail_bind("10.0.0.2", DISCOVERY_PORT, "Permission denied");
    auto refused = bus.transport("10.0.0.2");
    CHECK(refused->bind("0.0.0.0", DISCOVERY_PORT).error_or("") == "Permission denied");
    CHECK(refused->bind("0.0.0.0", DISCOVERY_PORT)); // the failure was for one bind

    first->close();
    CHECK_FALSE(first->receive(10ms));
    auto third = bus.transport("127.0.0.1");
    CHECK(third->bind("0.0.0.0", DISCOVERY_PORT)); // closing freed the port
}

TEST_CASE("dropped and delayed datagrams", "[discovery_bus]") {
    testing::FakeDiscoveryBus bus;
    auto host = bus.transport("192.168.1.20");
    REQUIRE(host->bind("", DISCOVERY_PORT));
    auto client = bus.transport("192.168.1.30");
    int seen = 0;
    bus.set_drop([&seen](const testing::FakeDiscoveryBus::Sent&, std::string_view) {
        return ++seen == 1; // UDP may drop any one of them
    });
    CHECK(client->send_to(bytes_of("first"), "192.168.1.20", DISCOVERY_PORT));
    CHECK(client->send_to(bytes_of("second"), "192.168.1.20", DISCOVERY_PORT));
    const auto got = host->receive(50ms);
    REQUIRE(got);
    CHECK(got->bytes == bytes_of("second"));
    const auto sent = bus.sent();
    REQUIRE(sent.size() == 2);
    CHECK_FALSE(sent[0].delivered);
    CHECK(sent[1].delivered);

    bus.set_drop(nullptr);
    bus.set_delay(80ms);
    CHECK(client->send_to(bytes_of("late"), "192.168.1.20", DISCOVERY_PORT));
    CHECK_FALSE(host->receive(20ms));
    CHECK(host->receive(500ms));
}

TEST_CASE("a closing socket ends a receive waiting on another thread", "[discovery_bus]") {
    testing::FakeDiscoveryBus bus;
    auto host = bus.transport("192.168.1.20");
    REQUIRE(host->bind("", DISCOVERY_PORT));
    std::optional<Datagram> result = Datagram{};
    std::thread waiter([&] { result = host->receive(5s); });
    std::this_thread::sleep_for(20ms);
    const auto started = std::chrono::steady_clock::now();
    host->close();
    waiter.join();
    CHECK_FALSE(result);
    CHECK(std::chrono::steady_clock::now() - started < 2s);
}

TEST_CASE("a factory makes sockets on its computer", "[discovery_bus]") {
    testing::FakeDiscoveryBus bus;
    const DiscoveryTransportFactory factory = bus.factory("192.168.1.30");
    auto host = bus.transport("192.168.1.20");
    REQUIRE(host->bind("", DISCOVERY_PORT));
    auto searcher = factory(true);
    CHECK(searcher->send_to(bytes_of("q"), "192.168.1.20", DISCOVERY_PORT));
    const auto got = host->receive(50ms);
    REQUIRE(got);
    CHECK(got->sender == "192.168.1.30");
    CHECK(bus.bound().size() == 2);
}
