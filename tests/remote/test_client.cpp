// remote/client.h against services/remote_asr/client.py: the client half of the recorded probe
// and pairing exchanges (tests/fixtures/transcripts/remote_probe, remote_pair_code,
// remote_pair_tailscale, remote_pair_approval) replayed by M9.3's ScriptedPeer, the ported tests
// of tests/test_remote_engine.py and tests/test_remote_pairing_ux.py, and the wording of each
// way a connection fails to open.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/cancel.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "client_test_support.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "net/ws_client.h"
#include "pyport/errors.h"
#include "remote/client.h"
#include "remote/detail/client_socket.h"
#include "remote/errors.h"
#include "remote/pairing_settings.h"
#include "remote/protocol.h"
#include "remote_test_support.h"
#include "settings/store.h"
#include "transcript_replay.h"

using namespace fv;
using namespace fv::remote;
using namespace fv::remote::test;
using fv::remote::test::TempFolder;
using namespace std::chrono_literals;

namespace {

std::string fingerprint_of(std::string_view pair) {
    std::string fp;
    for (int i = 0; i < 32; ++i) {
        fp += pair;
    }
    return fp;
}

// The message of what `call` throws as a RemoteEngineError; "" when it throws nothing.
template <class F> std::string engine_error(F&& call) {
    try {
        call();
    } catch (const RemoteEngineError& error) {
        return error.what();
    }
    return {};
}

std::string a_product() {
    return remote::detail::a_product();
}

} // namespace

// ---- recorded exchanges ----

TEST_CASE("probe_host sends what OpenWhisper's client sends and reads each answer",
          "[transcript]") {
    const auto transcript =
        recorded_steps("probe", {"available", "unavailable", "tailscale_owner"});
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript);
    for (const auto& step : transcript.setup["steps"]) {
        INFO(step["name"].get<std::string>());
        const Json& expected = step["outcomes"][0]["returned"];
        const ProbeResult result = probe_host(rig.env, "127.0.0.1", peer.port());
        CHECK(result.host_name == expected["host_name"].get<std::string>());
        CHECK(result.protocol == expected["protocol"].get<std::int64_t>());
        CHECK(result.engine == expected["engine"]);
        CHECK(result.tailscale_pairing == expected["tailscale_pairing"].get<bool>());
        CHECK(result.fingerprint == peer.fingerprint());
        CHECK(result.approval == expected["approval"].get<bool>());
        CHECK(result.compatible());
    }
    CHECK(peer.wait_finished(10s));
    CHECK(peer.mismatches() == std::vector<std::string>{});
}

TEST_CASE("pair_with_host sends the code's digits and reads every refusal and the pairing",
          "[transcript]") {
    const auto transcript =
        recorded_steps("pair_code", {"pairing_closed", "wrong_code", "fifth_wrong_code",
                                     "right_code", "spaced_code", "no_digits"});
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript);
    const auto pair = [&](std::string_view code) {
        return pair_with_host(rig.env, "127.0.0.1", peer.port(), code, "laptop");
    };
    const std::string closed = "Pairing isn't open on the host. Click \"Show a pairing code\" "
                               "there and enter the code it shows.";
    const std::string wrong = "That code doesn't match the one on the host.";

    CHECK(engine_error([&] { pair("246810"); }) == closed);
    CHECK(engine_error([&] { pair("111111"); }) == wrong);
    for (int i = 0; i < 4; ++i) {
        CHECK(engine_error([&] { pair("111111"); }) == wrong);
    }
    CHECK(engine_error([&] { pair("111111"); }) ==
          "Too many wrong codes. Start pairing again on the host.");
    CHECK(engine_error([&] { pair("246810"); }) == closed);

    const std::vector<std::string> alternates = {"192.168.1.20", "100.101.102.103"};
    const PairingResult first = pair("246810");
    CHECK(first.token == bound(peer, "<token#1>"));
    CHECK(first.device_id == bound(peer, "<device_id#1>"));
    CHECK(first.host_name == "studio");
    CHECK(first.fingerprint == peer.fingerprint());
    CHECK(first.alternates == alternates);
    CHECK(first.via == "code");

    // The code as people type it: spaces and dashes are not sent.
    const PairingResult spaced = pair(" 24-68 10 ");
    CHECK(spaced.token == bound(peer, "<token#2>"));
    CHECK(spaced.alternates == alternates);

    // No digits: refused before anything connects.
    const auto before = peer.connections();
    CHECK(engine_error([&] { pair(" - "); }) == "Enter the pairing code shown on the host.");
    CHECK(peer.connections() == before);

    CHECK(peer.wait_finished(10s));
    CHECK(peer.mismatches() == std::vector<std::string>{});
}

TEST_CASE("pair_with_host over Tailscale sends no code and reads each answer", "[transcript]") {
    const auto transcript =
        recorded_steps("pair_tailscale", {"no_owner", "not_owners_computer", "owners_computer"});
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript);
    const auto pair = [&] {
        return pair_with_host(rig.env, "127.0.0.1", peer.port(), "", "laptop", true);
    };
    CHECK(engine_error(pair) == "This host doesn't pair over Tailscale without a code. Enter the "
                                "pairing code shown there instead.");
    CHECK(engine_error(pair) == "This host only lets its owner's own Tailscale computers pair "
                                "without a code. Enter the pairing code shown there instead.");
    const PairingResult result = pair();
    CHECK(result.token == bound(peer, "<token#1>"));
    CHECK(result.via == "tailscale");
    CHECK(result.host_name == "studio");
    CHECK(result.fingerprint == peer.fingerprint());
    CHECK(peer.wait_finished(10s));
    CHECK(peer.mismatches() == std::vector<std::string>{});
}

TEST_CASE("request_pairing shows the number both screens show and reads every outcome",
          "[transcript]") {
    const auto transcript =
        recorded_steps("pair_approval", {"allowed", "denied", "timeout", "canceled",
                                         "broken_reveal", "refused", "limited", "busy", "stopped"});
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript);
    std::vector<std::string> shown;
    const auto request = [&](CancelToken cancel = {}, std::function<void()> then = {}) {
        return request_pairing(
            rig.env, "127.0.0.1", peer.port(), "laptop",
            [&](std::string sas) {
                shown.push_back(std::move(sas));
                if (then) {
                    then();
                }
            },
            cancel);
    };
    // The number the host computes from its nonce, this computer's and its own certificate.
    const auto host_sas = [&](const char* host_nonce, const char* client_nonce) {
        return pairing_sas(*remote::detail::from_hex(bound(peer, host_nonce)),
                           *remote::detail::from_hex(bound(peer, client_nonce)),
                           peer.fingerprint());
    };

    const PairingResult allowed = request();
    CHECK(allowed.token == bound(peer, "<token#1>"));
    CHECK(allowed.via == "approval");
    CHECK(allowed.alternates == std::vector<std::string>{"192.168.1.20", "100.101.102.103"});
    REQUIRE(shown.size() == 1);
    CHECK(shown[0] == host_sas("<nonce#2>", "<nonce#1>"));

    CHECK(engine_error([&] { request(); }) == "studio declined the request.");
    CHECK(engine_error([&] { request(); }) ==
          "Nobody answered on studio. Try again while someone is at it, or pair with a code.");

    CancelSource cancel;
    try {
        request(cancel.token(), [&] { cancel.cancel(); });
        FAIL("a canceled request paired");
    } catch (const PairingCanceled& error) {
        CHECK(std::string(error.what()) == "Pairing canceled.");
    }

    shown.clear();
    CHECK(engine_error([&] { request(); }) ==
          "The host's pairing answer didn't check out. Pairing was stopped; try again on a "
          "network you trust.");
    CHECK(shown.empty());

    CHECK(engine_error([&] { request(); }) ==
          "This host only takes pairing requests from its own network. Enter the pairing code "
          "shown there instead.");
    CHECK(engine_error([&] { request(); }) ==
          "Too many pairing requests from this computer. Wait a few minutes, or enter the "
          "pairing code shown on the host.");
    CHECK(engine_error([&] { request(); }) == "studio declined the request.");
    CHECK(engine_error([&] { request(); }) ==
          "studio is already asking about another computer. Try again in a minute.");
    CHECK(engine_error([&] { request(); }) == "studio stopped sharing its engine.");

    CHECK(peer.wait_finished(10s));
    CHECK(peer.mismatches() == std::vector<std::string>{});
}

// ---- tests/test_remote_pairing_ux.py ----

TEST_CASE("the probe names the certificate and approval", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_the_probe_names_the_certificate_and_approval
    ClientRig rig;
    MiniHost host(rig.reactor);
    const ProbeResult result = probe_host(rig.env, "127.0.0.1", host.port());
    CHECK(result.fingerprint == host.fingerprint());
    CHECK(result.approval);
    CHECK(result.host_name == "devbox");
    CHECK(result.engine["label"] == "Fake Parakeet");
}

TEST_CASE("a host whose reveal breaks its commitment is not trusted", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::
    // test_a_host_whose_reveal_breaks_its_commitment_is_not_trusted
    ClientRig rig;
    MiniHost liar(rig.reactor, {.broken_reveal = true});
    std::vector<std::string> shown;
    CHECK(engine_error([&] {
              request_pairing(rig.env, "127.0.0.1", liar.port(), "laptop",
                              [&](std::string sas) { shown.push_back(std::move(sas)); });
          }).find("didn't check out") != std::string::npos);
    CHECK(shown.empty()); // no number to compare was ever shown
}

// ---- tests/test_remote_engine.py ----

TEST_CASE("pairing keeps the host's other addresses", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_pairing_keeps_the_hosts_other_addresses
    TempFolder folder;
    MapEnvReader env;
    // Every variable the per-user folder is read from, inside the test's own folder.
    env.set("LOCALAPPDATA", path_to_utf8(folder / "local"));
    env.set("XDG_DATA_HOME", path_to_utf8(folder / "local"));
    env.set("HOME", path_to_utf8(folder / "home"));
    env.set("USERPROFILE", path_to_utf8(folder / "home"));
    const Paths paths = Paths::resolve(identity(), env);
    SettingsStore settings(paths.user_file(identity().settings_file));
    CredentialStore credentials{std::make_shared<MemoryCredentialBackend>(), identity()};
    FakeClock clock;

    const PairingResult result{
        "t", "d", "devbox", fingerprint_of("AB"), {"192.168.1.20", "100.64.0.2"}, "code"};
    const ClientPairing pairing = save_client_pairing(settings, credentials, paths, identity(),
                                                      "192.168.1.20", 47821, result, clock);
    CHECK(pairing.alternates == std::vector<std::string>{"100.64.0.2"});
    const auto loaded = load_client_pairing(settings);
    REQUIRE(loaded.has_value());
    CHECK(*loaded == pairing);
    CHECK(!loaded->over_tailscale());
    CHECK(loaded->tailscale_fallback() == "100.64.0.2");

    const ClientPairing over = save_client_pairing(
        settings, credentials, paths, identity(), "100.64.0.2", 47821,
        PairingResult{"t", "d", "devbox", fingerprint_of("AB"), {"192.168.1.20"}, "tailscale"},
        clock);
    CHECK(over.over_tailscale());
    CHECK(over.tailscale_fallback().empty());
    CHECK(over.via == "tailscale");
}

// ---- the pairing reply is checked against the certificate seen here ----

TEST_CASE("a pairing whose host reports another certificate is stopped") {
    ClientRig rig;
    MiniHost host(rig.reactor, {.advertised_fingerprint = fingerprint_of("CD")});
    CHECK(engine_error([&] {
              pair_with_host(rig.env, "127.0.0.1", host.port(), "246810", "laptop");
          }) == "The host's certificate didn't match what it reported. Pairing was stopped; try "
                "again on a network you trust.");
}

TEST_CASE("a reported certificate in lower case is the same certificate") {
    ClientRig rig;
    MiniHost host(rig.reactor, {.lowercase_fingerprint = true});
    const PairingResult result =
        pair_with_host(rig.env, "127.0.0.1", host.port(), "246810", "laptop");
    CHECK(result.fingerprint == host.fingerprint());
    CHECK(result.alternates == std::vector<std::string>{"192.0.2.10"});
    CHECK(result.host_name == "devbox");
}

// ---- how a connection that did not open is worded ----

TEST_CASE("each way a connection fails to open has its own sentence") {
    using Kind = net::WsConnectFailure::Kind;
    const auto said = [](Kind kind, int status = 0) -> std::pair<std::string, bool> {
        try {
            remote::detail::throw_open_failure(net::WsConnectFailure{kind, status, "detail"},
                                               "devbox", 5000);
        } catch (const RemoteUnreachable& error) {
            return {error.what(), error.refused};
        } catch (const RemoteEngineError& error) {
            return {std::string("engine: ") + error.what(), false};
        }
        return {};
    };
    const std::string product = std::string(identity().product);
    CHECK(said(Kind::HttpStatus, 404).first == "engine: devbox:5000 answered, but it isn't " +
                                                   a_product() + " remote engine (HTTP 404).");
    CHECK(said(Kind::Handshake).first ==
          "engine: devbox:5000 isn't " + a_product() + " remote engine.");
    CHECK(said(Kind::Dns).first == "Couldn't find a computer named devbox.");
    CHECK(said(Kind::Refused) ==
          std::pair<std::string, bool>{"Couldn't reach devbox:5000: nothing there is accepting "
                                       "connections on that port. Check that the host is "
                                       "sharing its engine, on that port.",
                                       true});
    const std::string plain = "Couldn't reach devbox:5000. Check that the host is on, sharing "
                              "its engine, and allowed through its firewall.";
    for (const Kind kind : {Kind::Unreachable, Kind::Timeout, Kind::Tls, Kind::Cancelled}) {
        CHECK(said(kind) == std::pair<std::string, bool>{plain, false});
    }
    // An IPv6 host is bracketed where the address is named, not where the name is.
    try {
        remote::detail::throw_open_failure(net::WsConnectFailure{Kind::Unreachable, 0, ""},
                                           "fe80::1", 47821);
    } catch (const RemoteUnreachable& error) {
        CHECK(std::string(error.what()).starts_with("Couldn't reach [fe80::1]:47821."));
    }
    CHECK(a_product() == (std::string("AEIOUaeiou").find(product.front()) == std::string::npos
                              ? "a " + product
                              : "an " + product));
}

TEST_CASE("a server that refuses the upgrade isn't a remote engine", "[transcript]") {
    // The recorded 404 of a path that is not the engine's, played where the client asks.
    const auto transcript = wire::load_transcript("wrong_path");
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript, {.path = "/elsewhere"});
    const std::string where = "127.0.0.1:" + std::to_string(peer.port());
    CHECK(engine_error([&] { probe_host(rig.env, "127.0.0.1", peer.port()); }) ==
          where + " answered, but it isn't " + a_product() + " remote engine (HTTP 404).");
}

TEST_CASE("nothing listening is a refusal or no answer, never a hang") {
    ClientRig rig;
    std::uint16_t port = 0;
    {
        MiniHost gone(rig.reactor);
        port = gone.port();
    }
    try {
        probe_host(rig.env, "127.0.0.1", port, 2s);
        FAIL("a probe of a closed port answered");
    } catch (const RemoteUnreachable& error) {
        CHECK(std::string(error.what())
                  .starts_with("Couldn't reach 127.0.0.1:" + std::to_string(port)));
    }
}

TEST_CASE("pair_with_host refuses a code with no digits in any script but sends other digits") {
    ClientRig rig;
    MiniHost host(rig.reactor, {.code = "\xd9\xa1\xd9\xa2\xd9\xa3"}); // Arabic-Indic 123
    const PairingResult result =
        pair_with_host(rig.env, "127.0.0.1", host.port(), "\xd9\xa1-\xd9\xa2 \xd9\xa3", "laptop");
    CHECK(!result.token.empty());
}

TEST_CASE("fromhex and compare_digest read as Python's do") {
    CHECK(remote::detail::from_hex("ab cd") == std::vector<std::uint8_t>{0xab, 0xcd});
    CHECK(remote::detail::from_hex(" ABcd\t") == std::vector<std::uint8_t>{0xab, 0xcd});
    CHECK(remote::detail::from_hex("") == std::vector<std::uint8_t>{});
    CHECK(!remote::detail::from_hex("a bc").has_value());
    CHECK(!remote::detail::from_hex("abc").has_value());
    CHECK(!remote::detail::from_hex("zz").has_value());
    CHECK(remote::detail::compare_digest("ABC", "ABC"));
    CHECK(!remote::detail::compare_digest("ABC", "ABD"));
    CHECK(!remote::detail::compare_digest("ABC", "ABCD"));
    CHECK_THROWS_AS(remote::detail::compare_digest("\xc3\xa9", "e"), py::TypeError);
}
