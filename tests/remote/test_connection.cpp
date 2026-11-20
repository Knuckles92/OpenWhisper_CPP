// remote/connection.h against services/remote_asr/client.py's RemoteConnection: the client half
// of the recorded hello, request and direct-history exchanges (tests/fixtures/transcripts/
// remote_hello_ready, remote_requests, remote_history) replayed by M9.3's ScriptedPeer, and the
// ported tests of tests/test_remote_engine.py and tests/test_remote_pairing_ux.py against a
// loopback host.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "base/json.h"
#include "base/testing/manual_executor.h"
#include "client_test_support.h"
#include "pyport/json.h"
#include "remote/client.h"
#include "remote/connection.h"
#include "remote/errors.h"
#include "remote/pairing_settings.h"
#include "remote/protocol.h"
#include "remote/remote_backend.h"
#include "remote_test_support.h"
#include "settings/store.h"
#include "transcript_replay.h"

using namespace fv;
using namespace fv::remote;
using namespace fv::remote::test;
using namespace std::chrono_literals;

namespace {

// What a call threw: its kind and message.
struct Raised {
    std::string kind;
    std::string message;
    std::optional<std::string> code;
    std::optional<double> retry_after_s;
    std::optional<bool> sent;
};

template <class F> std::optional<Raised> raised_by(F&& call) {
    try {
        call();
    } catch (const RemoteConnectionLost& error) {
        return Raised{"RemoteConnectionLost", error.what(), {}, {}, error.sent};
    } catch (const RemoteHostBusy& error) {
        return Raised{"RemoteHostBusy", error.what(), {}, error.retry_after_s, {}};
    } catch (const RemoteRequestError& error) {
        return Raised{"RemoteRequestError", error.what(), error.code, error.retry_after_s, {}};
    } catch (const RemoteEngineError& error) {
        return Raised{"RemoteEngineError", error.what(), {}, {}, {}};
    } catch (const std::runtime_error& error) {
        return Raised{"RuntimeError", error.what(), {}, {}, {}};
    }
    return std::nullopt;
}

// The fields of a recorded request header besides id and op, in their order.
Json fields_of(const Json& header) {
    Json fields = Json::object();
    for (const auto& [key, value] : header.items()) {
        if (key != "id" && key != "op") {
            fields[key] = value;
        }
    }
    return fields;
}

PairingResult pair(ClientRig& rig, const MiniHost& host) {
    return pair_with_host(rig.env, "127.0.0.1", host.port(), "246810", "laptop");
}

// Plays one recorded speech connection through a RemoteConnection: hello as recorded (with
// capabilities when the record has them), then each recorded request, each reply compared with
// the recorded one, then close().
void replay_requests(ClientRig& rig, const wire::Transcript& transcript,
                     const wire::ScriptedPeer& peer, int conn, const std::string& token,
                     const std::string& fingerprint) {
    INFO("conn " << conn);
    const auto hello = frames_where(transcript, conn, "c2s", "text");
    REQUIRE(!hello.empty());
    const Json recorded_hello = py::loads(transcript.frames[hello.front()].text);
    RemoteConnection connection(rig.env, "127.0.0.1", peer.port(), token, fingerprint, {}, {},
                                recorded_hello.contains("capabilities"));
    const Json ready = connection.connect();
    CHECK(ready == played(transcript, frames_where(transcript, conn, "s2c", "text").front(), peer));

    const auto requests = frames_where(transcript, conn, "c2s", "binary");
    const auto replies = frames_where(transcript, conn, "s2c", "text");
    if (requests.empty() && !frames_where(transcript, conn, "s2c", "close").empty()) {
        // The host closed after ready (device removed): the next request is not sent.
        REQUIRE(wait_for([&] { return !connection.alive(); }));
        const auto failure = raised_by([&] { connection.request("describe"); });
        REQUIRE(failure.has_value());
        CHECK(failure->kind == "RemoteConnectionLost");
        CHECK(failure->message == "Lost the connection to " + connection.where() + ".");
        CHECK(failure->sent == false);
        return;
    }
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& frame = transcript.frames[requests[i]];
        const Json header = py::loads(frame.text);
        const std::string op = header["op"].get<std::string>();
        const Json fields = fields_of(header);
        const Json reply = played(transcript, replies[i + 1], peer);
        INFO(op << " " << fields.dump());
        std::vector<float> audio;
        std::vector<std::byte> payload;
        if (!frame.payload.empty()) {
            if (op == "transcribe" || op == "stream") {
                audio = decode_audio(frame.payload);
            } else {
                payload = frame.payload;
            }
        }
        std::optional<Json> result;
        std::optional<double> host_s;
        const auto failure = raised_by([&] {
            if (op.starts_with("recording_") && payload.empty() && fields.size() == 1 &&
                fields.contains("recording_id")) {
                connection.recording_activity(op, fields["recording_id"].get<std::string>());
                result.emplace(Json::object());
            } else {
                auto [value, seconds] = connection.request_timed(op, fields, 10s, audio, payload);
                result.emplace(std::move(value));
                host_s = seconds;
            }
        });
        if (reply.contains("result")) {
            CHECK(!failure.has_value());
            REQUIRE(result.has_value());
            if (!op.starts_with("recording_")) {
                CHECK(*result == reply["result"]);
                CHECK(host_s.has_value());
            }
        } else if (reply.value("code", "") == "engine_changed") {
            REQUIRE(failure.has_value());
            CHECK(failure->kind == "RemoteConnectionLost");
            CHECK(failure->message == reply["error"].get<std::string>());
            CHECK(failure->sent == true);
            CHECK(connection.closed());
            CHECK(!connection.closed_for_engine_change()); // the socket was let go
        } else {
            REQUIRE(failure.has_value());
            CHECK(failure->kind == "RemoteRequestError");
            CHECK(failure->message == reply["error"].get<std::string>());
            if (reply.contains("code")) {
                CHECK(failure->code == reply["code"].get<std::string>());
            } else {
                CHECK(!failure->code.has_value());
            }
            if (reply.contains("retry_after_ms")) {
                CHECK(failure->retry_after_s == 1.0);
            }
        }
    }
    connection.close();
    CHECK(connection.closed());
}

} // namespace

// ---- recorded exchanges ----

TEST_CASE("hello says what OpenWhisper's client says and ready is read as sent", "[transcript]") {
    const auto transcript =
        recorded_steps("hello_ready", {"pair", "ready_full", "ready_plain", "unauthorized",
                                       "loading", "update_hold", "busy_connections"});
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript);
    const PairingResult paired =
        pair_with_host(rig.env, "127.0.0.1", peer.port(), "246810", "laptop");
    CHECK(paired.token == bound(peer, "<token#1>"));

    const auto connect = [&](int conn, const std::string& token, bool recording) {
        INFO("conn " << conn);
        RemoteConnection connection(rig.env, "127.0.0.1", peer.port(), token, paired.fingerprint,
                                    {}, {}, recording);
        const Json ready = connection.connect();
        CHECK(ready ==
              played(transcript, frames_where(transcript, conn, "s2c", "text").front(), peer));
        CHECK(connection.ready() == ready);
        CHECK(connection.host() == "127.0.0.1");
        connection.close();
    };
    connect(1, paired.token, true);
    connect(2, paired.token, false);
    {
        RemoteConnection stranger(rig.env, "127.0.0.1", peer.port(), "not-a-paired-token",
                                  paired.fingerprint);
        const auto failure = raised_by([&] { stranger.connect(); });
        REQUIRE(failure.has_value());
        CHECK(failure->kind == "RemoteEngineError");
        CHECK(failure->message ==
              "This computer isn't paired with the host anymore. Pair it again.");
    }
    connect(4, paired.token, true);
    connect(5, paired.token, true);
    connect(6, paired.token, false);
    {
        RemoteConnection second(rig.env, "127.0.0.1", peer.port(), paired.token,
                                paired.fingerprint);
        const auto failure = raised_by([&] { second.connect(); });
        REQUIRE(failure.has_value());
        CHECK(failure->kind == "RemoteHostBusy");
        CHECK(failure->message == "The host is busy. Try again shortly.");
        CHECK(failure->retry_after_s == 1.0);
    }
    CHECK(peer.wait_finished(10s));
    CHECK(peer.mismatches() == std::vector<std::string>{});
}

TEST_CASE("requests go out as OpenWhisper's client frames them and replies are read as sent",
          "[transcript]") {
    const auto transcript = recorded_steps(
        "requests", {"pair", "transcribe", "stream", "stream_refused", "engine_loading",
                     "update_hold", "busy", "engine_changed", "select_model", "configure_runtime",
                     "model_ops", "recording", "records_and_mcp", "device_removed"});
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript);
    const PairingResult paired =
        pair_with_host(rig.env, "127.0.0.1", peer.port(), "246810", "laptop");
    for (const int conn : wire::connections(transcript)) {
        if (conn == 0) {
            continue;
        }
        replay_requests(rig, transcript, peer, conn, paired.token, paired.fingerprint);
    }
    CHECK(peer.wait_finished(10s));
    CHECK(peer.mismatches() == std::vector<std::string>{});
}

TEST_CASE("the history channel says hello as OpenWhisper's and carries text both ways",
          "[transcript]") {
    const auto transcript =
        recorded_steps("history", {"pair", "connect", "queries", "over_limit", "stop"});
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript);
    const PairingResult paired =
        pair_with_host(rig.env, "127.0.0.1", peer.port(), "246810", "laptop");
    RemoteConnection connection(rig.env, "127.0.0.1", peer.port(), paired.token, paired.fingerprint,
                                {}, true);
    const Json ready = connection.connect();
    CHECK(ready == Json{{"type", "ready"}, {"capabilities", {{"client_history", true}}}});
    for (const std::size_t answer : frames_where(transcript, 1, "c2s", "text")) {
        if (answer == frames_where(transcript, 1, "c2s", "text").front()) {
            continue; // the hello
        }
        std::optional<std::string> query;
        REQUIRE(wait_for([&] {
            query = connection.receive_history(100ms);
            return query.has_value();
        }));
        CHECK(py::loads(*query).value("type", "") == "history_query");
        Json bindings = peer.bindings();
        connection.send_history(wire::render(transcript, answer, bindings).text);
    }
    connection.close();
    CHECK_THROWS_AS(connection.receive_history(10ms), RemoteEngineError);
    CHECK(peer.wait_finished(10s));
    CHECK(peer.mismatches() == std::vector<std::string>{});
}

// ---- tests/test_remote_engine.py ----

TEST_CASE("a different certificate is refused before the token is sent", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::
    // test_a_different_certificate_is_refused_before_the_token_is_sent
    ClientRig rig;
    MiniHost host(rig.reactor);
    const PairingResult result = pair(rig, host);
    MiniHost impostor(rig.reactor);
    RemoteConnection connection(rig.env, "127.0.0.1", impostor.port(), result.token,
                                result.fingerprint);
    const auto failure = raised_by([&] { connection.connect(); });
    REQUIRE(failure.has_value());
    CHECK(failure->message.find("different certificate") != std::string::npos);
    CHECK(failure->message.find("If " + std::string(identity().product) +
                                " was reinstalled there") != std::string::npos);
    CHECK(impostor.hellos().empty());
}

TEST_CASE("close from another thread cancels a waiting request", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_close_from_another_thread_cancels_a_waiting_request
    ClientRig rig;
    MiniHost host(rig.reactor, {.transcribe_delay = 1000ms});
    const PairingResult result = pair(rig, host);
    RemoteConnection connection(rig.env, "127.0.0.1", host.port(), result.token,
                                result.fingerprint);
    connection.connect();
    std::optional<Raised> failure;
    std::thread worker([&] {
        failure = raised_by([&] { connection.request("transcribe", {}, 180s, tone(160)); });
    });
    REQUIRE(wait_for([&] { return !host.requests().empty(); }, 3s));
    connection.close();
    worker.join();
    REQUIRE(failure.has_value());
    CHECK(failure->message == "Transcription canceled");
}

TEST_CASE("the keepalive times pongs and closes on a missed one", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_the_keepalive_times_pongs_and_closes_on_a_missed_one
    ClientRig rig;
    rig.env.keepalive_interval = 50ms;
    rig.env.keepalive_timeout = 300ms;
    // After hello the host stops reading: the request below is never answered, and the pings
    // behind it are never read, so their pongs never come.
    MiniHost host(rig.reactor, {.stop_reading_after = 1});
    const PairingResult result = pair(rig, host);
    RemoteConnection connection(rig.env, "127.0.0.1", host.port(), result.token,
                                result.fingerprint);
    connection.connect();
    // One ping goes out on connect, timed finely enough for a home network.
    REQUIRE(wait_for([&] { return connection.beats() >= 2; }));
    REQUIRE(connection.latency().has_value());
    CHECK(*connection.latency() > 0);
    CHECK(*connection.latency() < 1);
    CHECK(connection.alive());

    std::optional<Raised> abandoned;
    std::thread worker([&] { abandoned = raised_by([&] { connection.request("describe"); }); });
    REQUIRE(wait_for([&] { return host.requests().size() == 1; }, 3s));
    connection.abandon();
    worker.join();
    REQUIRE(abandoned.has_value());
    CHECK(abandoned->message == "Transcription canceled");
    // A host that stops answering pings is closed, as websockets' own keepalive would.
    CHECK(wait_for([&] { return !connection.alive(); }, 3s));
    CHECK(connection.closed());
}

TEST_CASE("an unreachable host reports plainly", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_unreachable_host_reports_plainly
    ClientRig rig;
    std::uint16_t port = 0;
    {
        MiniHost gone(rig.reactor);
        port = gone.port();
    }
    RemoteConnection connection(rig.env, "127.0.0.1", port, "t", std::string(64, 'F'), {}, {},
                                false, 2s);
    const auto failure = raised_by([&] { connection.connect(); });
    REQUIRE(failure.has_value());
    CHECK(failure->message.starts_with("Couldn't reach"));
}

TEST_CASE("cancel stops waiting but keeps the connection", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::test_cancel_stops_waiting_but_keeps_the_connection (the
    // connection's half; the backend's is RemoteSpeechBackend's)
    ClientRig rig;
    MiniHost host(rig.reactor, {.transcribe_delay = 600ms});
    const PairingResult result = pair(rig, host);
    RemoteConnection connection(rig.env, "127.0.0.1", host.port(), result.token,
                                result.fingerprint);
    connection.connect();
    std::optional<Raised> failure;
    const bool abandon = GENERATE(true, false);
    CancelSource cancel;
    std::jthread worker([&] {
        failure = raised_by([&] {
            connection.request_timed("transcribe", {}, 180s, tone(1600), {}, cancel.token());
        });
    });
    const bool sent = wait_for([&] { return !host.requests().empty(); }, 3s);
    if (abandon) {
        connection.abandon();
    } else {
        cancel.cancel();
    }
    worker.join();
    REQUIRE(sent);
    REQUIRE(failure.has_value());
    CHECK(failure->message == "Transcription canceled");
    CHECK(!connection.closed());
    CHECK(connection.alive());
    // The late reply to the abandoned request is skipped by the next one.
    const Json described = connection.request("describe");
    CHECK(described["model"] == "parakeet-v3");
    const Json heard = connection.request("transcribe", {}, 180s, tone(160));
    CHECK(heard["text"] == "heard 160");
}

TEST_CASE("a connection skips a stranger at the first address without sending the token",
          "[test_remote_engine]") {
    // from tests/test_remote_engine.py::
    // test_connection_skips_a_stranger_at_the_first_address_without_sending_the_token
    ClientRig rig;
    MiniHost host(rig.reactor);
    const PairingResult result = pair(rig, host);
    MiniHost stranger(rig.reactor, {.name = "stranger", .bind = "127.0.0.2", .port = host.port()});
    if (!stranger.listening()) {
        SKIP("127.0.0.2 is not usable on this machine: " << stranger.error());
    }
    {
        RemoteConnection connection(rig.env, "127.0.0.2", host.port(), result.token,
                                    result.fingerprint, {"127.0.0.1"});
        const Json ready = connection.connect();
        CHECK(ready["host"]["name"] == "devbox");
        CHECK(connection.host() == "127.0.0.1");
        CHECK(connection.request("transcribe", {}, 180s, tone(160))["text"] == "heard 160");
        connection.close();
    }
    // The stranger saw a TLS handshake and an upgrade, nothing else: no hello, no token.
    CHECK(wait_for([&] { return stranger.connections() == 1; }));
    std::this_thread::sleep_for(200ms);
    CHECK(stranger.hellos().empty());

    // The address that worked is tried first from now on.
    RemoteConnection again(rig.env, "127.0.0.2", host.port(), result.token, result.fingerprint,
                           {"127.0.0.1"});
    again.connect();
    CHECK(again.host() == "127.0.0.1");
    CHECK(stranger.connections() == 1);
    again.close();
}

TEST_CASE("a connection reports the paired address when nothing answers", "[test_remote_engine]") {
    // from tests/test_remote_engine.py::
    // test_connection_reports_the_paired_address_when_nothing_answers
    ClientRig rig;
    std::uint16_t port = 0;
    PairingResult result;
    {
        MiniHost host(rig.reactor);
        port = host.port();
        result = pair(rig, host);
    }
    RemoteConnection connection(rig.env, "127.0.0.1", port, result.token, result.fingerprint, {},
                                {}, false, 1s);
    const auto failure = raised_by([&] { connection.connect(); });
    REQUIRE(failure.has_value());
    CHECK(failure->kind == "RemoteEngineError");
    CHECK(failure->message.find("127.0.0.1:" + std::to_string(port)) != std::string::npos);
}

TEST_CASE("the error raised is the paired address's, not an alternate's") {
    ClientRig rig;
    MiniHost host(rig.reactor);
    const PairingResult result = pair(rig, host);
    MiniHost impostor(rig.reactor);
    // Paired at the impostor's port; the alternate is the same port at an address where nothing
    // listens.
    RemoteConnection connection(rig.env, "127.0.0.1", impostor.port(), result.token,
                                result.fingerprint, {"127.0.0.3"}, {}, false, 2s);
    const auto failure = raised_by([&] { connection.connect(); });
    REQUIRE(failure.has_value());
    CHECK(failure->message.find("different certificate") != std::string::npos);
    CHECK(failure->message.starts_with("127.0.0.1:"));
}

// ---- tests/test_remote_pairing_ux.py ----

TEST_CASE("a connection finds the host where it answers now and saves it",
          "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::
    // test_a_connection_finds_the_host_where_it_answers_now_and_saves_it
    ClientRig rig;
    TempFolder folder;
    SettingsStore settings(folder / "settings.json");
    rig.env.remember_addresses = [&](std::string_view fingerprint, std::string_view reached,
                                     int port, std::span<const std::string> listed) {
        remember_host_addresses(settings, fingerprint, reached, port, listed);
    };
    auto host = std::make_unique<MiniHost>(rig.reactor);
    const PairingResult result = pair(rig, *host);
    std::uint16_t stale_port = 0;
    {
        MiniHost gone(rig.reactor);
        stale_port = gone.port();
    }
    // The saved pairing points where the host no longer is.
    ClientPairing saved;
    saved.host = "127.0.0.1";
    saved.port = stale_port;
    saved.fingerprint = result.fingerprint;
    saved.host_name = result.host_name;
    saved.device_id = result.device_id;
    saved.paired_at = "2026-11-17T12:00:00+00:00";
    write_pairing(settings, saved);
    BusResponder responder(rig.bus, "127.0.0.1",
                           Json{{"name", "devbox"},
                                {"port", host->port()},
                                {"fingerprint", host->fingerprint()},
                                {"approval", true},
                                {"engine", Json{{"label", "Fake Parakeet"}, {"available", true}}}});
    RemoteConnection::clear_route_memory();

    RemoteConnection connection(rig.env, "127.0.0.1", stale_port, result.token, result.fingerprint,
                                {}, {}, false, 1s);
    const Json ready = connection.connect();
    CHECK(ready["host"]["name"] == "devbox");
    CHECK(connection.port() == host->port());
    connection.close();
    const auto loaded = load_client_pairing(settings);
    REQUIRE(loaded.has_value());
    CHECK(loaded->port == host->port());
    CHECK(loaded->alternates == std::vector<std::string>{"192.0.2.10"});

    // A host that is just off isn't searched for on every retry.
    const auto queries = [&] {
        std::set<std::vector<std::byte>> distinct;
        for (const auto& sent : rig.bus.sent()) {
            if (sent.from == "10.0.0.2") {
                distinct.insert(sent.bytes);
            }
        }
        return distinct.size();
    };
    const std::size_t before = queries();
    const std::uint16_t was = host->port();
    host.reset();
    for (int i = 0; i < 2; ++i) {
        RemoteConnection retry(rig.env, "127.0.0.1", was, result.token, result.fingerprint, {}, {},
                               false, 1s);
        CHECK(raised_by([&] { retry.connect(); }).has_value());
    }
    CHECK(queries() - before <= 1);
}

TEST_CASE("a forged answer gets no token", "[test_remote_pairing_ux]") {
    // from tests/test_remote_pairing_ux.py::test_a_forged_answer_gets_no_token
    ClientRig rig;
    auto host = std::make_unique<MiniHost>(rig.reactor);
    const PairingResult result = pair(rig, *host);
    const std::uint16_t was = host->port();
    MiniHost stranger(rig.reactor, {.name = "stranger"});
    // Discovery says the host is at the stranger's port.
    BusResponder responder(rig.bus, "127.0.0.1",
                           Json{{"name", "devbox"},
                                {"port", stranger.port()},
                                {"fingerprint", result.fingerprint},
                                {"approval", true},
                                {"engine", Json::object()}});
    RemoteConnection::clear_route_memory();
    host.reset();
    RemoteConnection connection(rig.env, "127.0.0.1", was, result.token, result.fingerprint, {}, {},
                                false, 1s);
    CHECK(raised_by([&] { connection.connect(); }).has_value());
    CHECK(responder.answered() >= 1);
    std::this_thread::sleep_for(200ms);
    CHECK(stranger.connections() == 1);
    CHECK(stranger.hellos().empty());
    CHECK(connection.port() == was);
}

// ---- the rest of RemoteConnection ----

TEST_CASE("a full host's refusal before hello still reaches the caller", "[transcript]") {
    // hello_ready's busy_handshakes: the host refuses and closes as the connection opens, before
    // it reads anything. The hello that follows is no error; the queued refusal is the answer.
    const auto recorded = wire::load_transcript("hello_ready");
    const std::vector<int> refused = {9};
    const auto transcript = wire::only_connections(recorded, refused);
    ClientRig rig;
    wire::ScriptedPeer peer(rig.reactor, peer_identity(), transcript);
    RemoteConnection connection(rig.env, "127.0.0.1", peer.port(), "token", peer.fingerprint());
    const auto failure = raised_by([&] { connection.connect(); });
    REQUIRE(failure.has_value());
    CHECK(failure->kind == "RemoteHostBusy");
    CHECK(failure->message == "The host is busy. Try again shortly.");
    CHECK(failure->retry_after_s == 1.0);
    CHECK(peer.wait_finished(10s));
    CHECK(peer.mismatches() == std::vector<std::string>{});
}

TEST_CASE("a host that switched engines closes an idle connection with 4409") {
    ClientRig rig;
    MiniHost host(rig.reactor);
    const PairingResult result = pair(rig, host);
    RemoteConnection connection(rig.env, "127.0.0.1", host.port(), result.token,
                                result.fingerprint);
    connection.connect();
    CHECK(!connection.closed_for_engine_change());
    host.close_all(CLOSE_ENGINE_CHANGED, "engine changed");
    REQUIRE(wait_for([&] { return !connection.alive(); }));
    CHECK(connection.closed_for_engine_change());
    CHECK(!connection.closed());
    const auto failure = raised_by([&] { connection.request("describe"); });
    REQUIRE(failure.has_value());
    CHECK(failure->kind == "RemoteConnectionLost");
    CHECK(failure->sent == false);
    // Closed here now, the socket let go: no longer an engine change to report.
    CHECK(connection.closed());
    CHECK(!connection.closed_for_engine_change());
}

TEST_CASE("readers describe the connection without sending anything") {
    ClientRig rig;
    MiniHost host(rig.reactor);
    const PairingResult result = pair(rig, host);
    RemoteConnection connection(rig.env, "127.0.0.1", host.port(), result.token, result.fingerprint,
                                {"", "127.0.0.1", "192.0.2.10"});
    CHECK(connection.where() == "127.0.0.1:" + std::to_string(host.port()));
    CHECK(!connection.alive());
    CHECK(!connection.closed());
    CHECK(connection.ready() == Json::object());
    const Json ready = connection.connect();
    CHECK(connection.connect() == ready);
    CHECK(host.hellos().size() == 1);
    CHECK(connection.alive());
    CHECK(!connection.via_tailscale());
    CHECK(!connection.closed_for_engine_change());
    CHECK_THROWS_AS(connection.receive_history(10ms), RemoteEngineError);
    CHECK_THROWS_AS(connection.send_history("{}"), RemoteEngineError);
    connection.close();
    connection.close();
    CHECK(connection.closed());
    CHECK(!connection.alive());
    const auto failure = raised_by([&] { connection.request("describe"); });
    REQUIRE(failure.has_value());
    CHECK(failure->kind == "RemoteConnectionLost");
    CHECK(failure->sent == false);
}

TEST_CASE("a request past its timeout closes the connection") {
    ClientRig rig;
    MiniHost host(rig.reactor, {.transcribe_delay = 1500ms});
    const PairingResult result = pair(rig, host);
    RemoteConnection connection(rig.env, "127.0.0.1", host.port(), result.token,
                                result.fingerprint);
    connection.connect();
    const auto failure = raised_by([&] { connection.request("transcribe", {}, 200ms, tone(160)); });
    REQUIRE(failure.has_value());
    CHECK(failure->kind == "RuntimeError");
    CHECK(failure->message ==
          "The remote engine on " + connection.where() + " timed out; the connection was closed.");
    CHECK(connection.closed());
}

TEST_CASE("recording activity waits only briefly for a busy connection") {
    ClientRig rig;
    MiniHost host(rig.reactor, {.transcribe_delay = 1500ms});
    const PairingResult result = pair(rig, host);
    RemoteConnection connection(rig.env, "127.0.0.1", host.port(), result.token, result.fingerprint,
                                {}, {}, true);
    connection.connect();
    connection.recording_activity("recording_begin", "0123456789abcdef0123456789abcdef");
    const auto begun = host.requests();
    REQUIRE(begun.size() == 1);
    CHECK(begun[0] == Json{{"id", 1},
                           {"op", "recording_begin"},
                           {"recording_id", "0123456789abcdef0123456789abcdef"}});
    std::thread worker(
        [&] { (void)raised_by([&] { connection.request("transcribe", {}, 180s, tone(160)); }); });
    REQUIRE(wait_for([&] { return host.requests().size() == 2; }, 3s));
    const auto started = std::chrono::steady_clock::now();
    const auto failure = raised_by([&] {
        connection.recording_activity("recording_end", "0123456789abcdef0123456789abcdef");
    });
    const auto waited = std::chrono::steady_clock::now() - started;
    REQUIRE(failure.has_value());
    CHECK(failure->message == "The remote speech connection is busy. Try again shortly.");
    CHECK(waited >= 700ms);
    CHECK(waited < 2500ms);
    worker.join();
}

TEST_CASE("hello carries capabilities only for a backend that reports recordings") {
    ClientRig rig;
    MiniHost host(rig.reactor);
    const PairingResult result = pair(rig, host);
    {
        RemoteConnection plain(rig.env, "127.0.0.1", host.port(), result.token, result.fingerprint);
        plain.connect();
    }
    {
        RemoteConnection recording(rig.env, "127.0.0.1", host.port(), result.token,
                                   result.fingerprint, {}, {}, true);
        recording.connect();
    }
    {
        // A history connection never says it records, whatever it is told.
        RemoteConnection history(rig.env, "127.0.0.1", host.port(), result.token,
                                 result.fingerprint, {}, false, true);
        history.connect();
    }
    const auto hellos = host.hellos();
    REQUIRE(hellos.size() == 3);
    CHECK(hellos[0] == Json{{"type", "hello"}, {"protocol", 1}, {"token", result.token}});
    CHECK(hellos[1] == Json{{"type", "hello"},
                            {"protocol", 1},
                            {"token", result.token},
                            {"capabilities", {{"recording_activity", true}}}});
    CHECK(hellos[2] == Json{{"type", "hello"},
                            {"protocol", 1},
                            {"token", result.token},
                            {"purpose", "history"},
                            {"history_enabled", false}});
}

TEST_CASE("Authenticated dictation reports a lost live connection without opening another",
          "[test_remote_engine]") {
    ClientRig rig;
    MiniHost host(rig.reactor);
    const auto approved = pair(rig, host);
    ClientPairing pairing;
    pairing.host = "127.0.0.1";
    pairing.port = host.port();
    pairing.fingerprint = approved.fingerprint;
    pairing.host_name = "Synthetic host";
    ManualExecutor notify;
    speech::SpeechService service(
        [&](const speech::BackendConfig&) {
            return std::make_unique<RemoteSpeechBackend>(
                [&] { return std::optional(pairing); },
                [&](const ClientPairing& peer) {
                    return std::make_unique<RemoteConnection>(rig.env, peer.host, peer.port,
                                                              approved.token, peer.fingerprint);
                },
                system_clock(), nullptr, true);
        },
        notify);
    service.select(speech::Slot::Dictation, {.backend = "remote"});
    REQUIRE(wait_for([&] {
        notify.run_all();
        return service.readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready;
    }));
    const auto connections = host.connections();
    host.close_all(4409, "engine changed");
    REQUIRE(wait_for([&] {
        notify.run_all();
        return service.readiness(speech::Slot::Dictation).state == speech::ReadyState::Offline;
    }));
    // transcriber/remote_backend.py check_link's sentence for close code 4409.
    CHECK(service.readiness(speech::Slot::Dictation).message ==
          "devbox switched engines. Reconnecting...");
    CHECK(host.connections() == connections);
    CHECK(host.requests().empty());
}

namespace {
struct BackendFixture {
    ClientRig rig;
    MiniHost host;
    PairingResult approved;
    ClientPairing pairing;
    RemoteConnection* first = nullptr;
    RemoteSpeechBackend backend;

    explicit BackendFixture(std::size_t busy = 0)
        : host(rig.reactor,
               [&] {
                   MiniHost::Options options;
                   options.transcribe_busy_count = busy;
                   return options;
               }()),
          approved(pair(rig, host)), pairing([&] {
              ClientPairing p;
              p.host = "127.0.0.1";
              p.port = host.port();
              p.fingerprint = approved.fingerprint;
              p.host_name = "Synthetic host";
              return p;
          }()),
          backend([&] { return std::optional(pairing); },
                  [&](const ClientPairing& peer) {
                      auto connection = std::make_unique<RemoteConnection>(
                          rig.env, peer.host, peer.port, approved.token, peer.fingerprint);
                      if (!first)
                          first = connection.get();
                      return connection;
                  },
                  system_clock()) {}

    speech::DecodeJob job() {
        auto samples = std::make_shared<const std::vector<float>>(tone(16000));
        speech::DecodeJob result;
        result.input = speech::AudioClip{samples, 0, samples->size(), 16000};
        return result;
    }
    std::size_t transcribes() const {
        const auto requests = host.requests();
        return std::count_if(requests.begin(), requests.end(), [](const Json& request) {
            return request.value("op", "") == "transcribe";
        });
    }
};
} // namespace

TEST_CASE("remote backend reconnects once after a host restart before sending",
          "[test_remote_engine]") {
    BackendFixture fixture;
    REQUIRE(fixture.backend.load(CancelToken{}).state == speech::ReadyState::Ready);
    REQUIRE(fixture.first);
    const auto connections = fixture.host.connections();
    fixture.host.restart();
    REQUIRE(fixture.host.listening());
    REQUIRE(wait_for([&] { return !fixture.first->alive(); }));
    CHECK(fixture.backend.decode(fixture.job()).transcript.text == "heard 16000");
    CHECK(fixture.host.connections() == connections + 1);
    CHECK(fixture.transcribes() == 1);
}

TEST_CASE("remote backend retries two busy refusals and succeeds", "[test_remote_engine]") {
    BackendFixture fixture(2);
    REQUIRE(fixture.backend.load(CancelToken{}).state == speech::ReadyState::Ready);
    CHECK(fixture.backend.decode(fixture.job()).transcript.text == "heard 16000");
    CHECK(fixture.transcribes() == 3);
}

TEST_CASE("canceling remote decode interrupts busy backoff", "[test_remote_engine]") {
    BackendFixture fixture(100);
    REQUIRE(fixture.backend.load(CancelToken{}).state == speech::ReadyState::Ready);
    std::optional<Raised> failure;
    std::atomic<bool> finished = false;
    std::thread worker([&] {
        failure = raised_by([&] { (void)fixture.backend.decode(fixture.job()); });
        finished = true;
    });
    const bool sent = wait_for([&] { return fixture.host.busy_replies() >= 1; });
    // The first retry is delayed by at least one second. Give the loopback response time
    // to reach the client, then confirm the decode remains pending before canceling.
    std::this_thread::sleep_for(200ms);
    const bool pending = !finished.load();
    const auto start = std::chrono::steady_clock::now();
    fixture.backend.cancel_transcription();
    worker.join();
    CHECK(sent);
    CHECK(pending);
    REQUIRE(failure);
    CHECK(failure->message.find("cancel") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - start < 1s);
    CHECK(fixture.transcribes() == 1);
}
