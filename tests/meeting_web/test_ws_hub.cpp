// meeting_web/ws_hub.h over a loopback server: tests/test_meeting_ws.py, the fan-out tests of
// tests/test_meeting_custom_report.py, and what the hub adds around them (invalidation, the
// connection cap, a replaced store, logs without tokens).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "hub_harness.h"
#include "meeting_web/ws_protocol.h"
#include "pyport/json.h"

using fv::Json;
using namespace fv::meeting_web::test_support;
namespace mw = fv::meeting_web;
using namespace std::chrono_literals;

namespace {

const Json kActivityTick = {{"kind", "thinking"},
                            {"label", "Model is thinking through the transcript…"},
                            {"tool", ""},
                            {"pass_kind", "consolidation"},
                            {"ts", "2026-08-16T12:00:00+00:00"}};

Json with_type(std::string_view type, const Json& body) {
    Json message = {{"type", type}};
    for (const auto& [key, value] : body.items()) {
        message[key] = value;
    }
    return message;
}

Json guest_action(Harness& app, const Json& op) {
    auto ws = app.guest("Mallory", "g_authz");
    ws->receive_until("hello");
    ws->send({{"type", "action"}, {"client_action_id", "ca_1"}, {"op", op}});
    return ws->receive_until("action_result");
}

} // namespace

// ---- TestWsHello ----

// from tests/test_meeting_ws.py::TestWsHello::test_hello_snapshot_shape
TEST_CASE("hello snapshot shape", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.host();
    const Json hello = ws->receive();
    CHECK(hello["type"] == "hello");
    CHECK(hello["role"] == "host");
    CHECK(hello["participant_id"] == "p_me");
    CHECK(hello["seq"] == 3);
    CHECK(hello["state"]["title"] == "WS Meeting");
    CHECK(hello["state"]["topic"]["current"] == "Kickoff");
    REQUIRE(hello["segments"].is_array());
    CHECK(hello["segments"][0]["id"] == "sg_1");
    CHECK(hello["meeting"]["id"] == "m_ws");
    CHECK(hello["meeting"]["title"] == "WS Meeting");
}

// from tests/test_meeting_ws.py::TestWsHello::test_hello_leaks_no_tokens_beyond_the_guest_link
TEST_CASE("hello leaks no tokens beyond the guest link", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.host();
    Json hello = ws->receive();
    CHECK(hello.dump().find(kHostToken) == std::string::npos);
    const std::string guest = hello["urls"]["guest"].get<std::string>();
    CHECK(guest.ends_with(kGuestToken));
    hello.erase("urls");
    CHECK(hello.dump().find(kGuestToken) == std::string::npos);
}

// from tests/test_meeting_ws.py::TestWsHello::test_patch_during_hello_window_is_delivered
TEST_CASE("patch during hello window is delivered", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    app.repo->on_get_last_segments([&] {
        app.store->apply(
            "host", std::nullopt,
            Json::array({Json{{"op", "add_item"}, {"card", "user_notes"}, {"text", "LOST"}}}));
    });
    auto ws = app.host();
    const Json hello = ws->receive();
    const Json patch = ws->receive();
    CHECK(hello["type"] == "hello");
    // Not in the snapshot...
    CHECK(hello["state"]["cards"]["user_notes"] == Json::array());
    // ...therefore it must arrive as a patch, or the client diverges.
    CHECK(patch["type"] == "patch");
    CHECK(patch["results"][0]["effect"]["item"]["text"] == "LOST");
    CHECK(patch["seq"] == 4);
}

// ---- TestAgentActivityWs ----

// from
// tests/test_meeting_ws.py::TestAgentActivityWs::test_host_receives_agent_activity_guest_does_not
TEST_CASE("host receives agent activity guest does not", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto host = app.host();
    host->receive_until("hello");
    auto guest = app.guest("Sam");
    guest->receive_until("hello");
    // Join is broadcast to every ready socket, including this guest.
    CHECK(guest->receive_until("presence")["event"] == "joined");
    CHECK(host->receive_until("presence")["event"] == "joined");
    app.server->broadcast(with_type("agent_activity", kActivityTick), true);
    const Json host_msg = host->receive_until("agent_activity");
    CHECK(host_msg["kind"] == "thinking");
    CHECK(host_msg["label"] == kActivityTick["label"]);
    CHECK(host_msg["tool"] == "");
    CHECK(host_msg["pass_kind"] == "consolidation");
    CHECK(host_msg["ts"] == kActivityTick["ts"]);
    guest->send({{"type", "ping"}});
    CHECK(guest->receive()["type"] == "pong");
}

// from tests/test_meeting_ws.py::TestAgentActivityWs::test_host_hello_carries_recent_ticks
TEST_CASE("host hello carries recent ticks", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    const Json older = kActivityTick;
    const Json newer = {{"kind", "tool"},
                        {"label", "Updating the dashboard…"},
                        {"tool", "patch_state"},
                        {"pass_kind", "cards"},
                        {"ts", "2026-08-16T12:00:01+00:00"}};
    app.engine->set_activity(Json::array({older, newer}));
    auto ws = app.host();
    const Json hello = ws->receive();
    CHECK(hello["type"] == "hello");
    CHECK(hello["role"] == "host");
    CHECK(hello["agent_activity"] == Json::array({older, newer}));
    CHECK_FALSE(hello["agent_activity"][0].contains("type"));
    CHECK(hello["agent_activity"][0]["tool"] == "");
    CHECK(hello["agent_activity"][1]["tool"] == "patch_state");
}

TEST_CASE("a host hello carries the last 50 ticks", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    Json ticks = Json::array();
    for (int i = 0; i < 60; ++i) {
        ticks.push_back(Json{{"kind", "tool"}, {"label", std::to_string(i)}});
    }
    app.engine->set_activity(ticks);
    auto ws = app.host();
    const Json hello = ws->receive();
    REQUIRE(hello["agent_activity"].size() == 50);
    CHECK(hello["agent_activity"][0]["label"] == "10");
    CHECK(hello["agent_activity"][49]["label"] == "59");
}

// from tests/test_meeting_ws.py::TestAgentActivityWs::test_guest_hello_omits_agent_activity_key
TEST_CASE("guest hello omits agent activity key", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    app.engine->set_activity(Json::array({kActivityTick}));
    auto ws = app.guest("Sam");
    const Json hello = ws->receive();
    CHECK(hello["type"] == "hello");
    CHECK(hello["role"] == "guest");
    CHECK_FALSE(hello.contains("agent_activity"));
}

// from tests/test_meeting_ws.py::TestAgentActivityWs::test_host_only_tick_skips_not_ready_host
TEST_CASE("host only tick skips not ready host", "[test_meeting_ws]") {
    // Python inspects the pending buffer of a socket that is not ready; here the broadcasts
    // happen while the host's hello is being assembled, and the socket shows what was buffered.
    Harness app(EngineKind::Canned);
    app.repo->on_get_last_segments([&] {
        app.server->broadcast(with_type("agent_activity", kActivityTick), true);
        app.server->broadcast(Json{{"type", "status"}, {"status", "active"}}, false);
    });
    auto ws = app.host();
    CHECK(ws->receive()["type"] == "hello");
    CHECK(ws->receive()["type"] == "status");
    ws->send({{"type", "ping"}});
    CHECK(ws->receive()["type"] == "pong");
}

// ---- TestWsActionRoundTrip ----

// from tests/test_meeting_ws.py::TestWsActionRoundTrip::test_action_echoes_client_action_id
TEST_CASE("action echoes client action id", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.host();
    CHECK(ws->receive()["type"] == "hello");
    ws->send({{"type", "action"},
              {"client_action_id", "ca_abc123"},
              {"op", {{"op", "add_item"}, {"card", "user_notes"}, {"text", "note"}}}});
    const Json result = ws->receive();
    CHECK(result["type"] == "action_result");
    CHECK(result["client_action_id"] == "ca_abc123");
    CHECK(result["results"][0]["ok"] == true);
    CHECK(result["results"][0]["seq"] == 4);
    const auto calls = app.engine->calls("apply_client_action");
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].args[0] == "host");
    CHECK(calls[0].args[2]["text"] == "note");
}

// from tests/test_meeting_ws.py::TestWsActionRoundTrip::test_ping_pong
TEST_CASE("ping pong", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.host();
    ws->receive();
    ws->send({{"type", "ping"}});
    CHECK(ws->receive()["type"] == "pong");
}

// from tests/test_meeting_ws.py::TestWsActionRoundTrip::test_guest_requires_name
TEST_CASE("guest requires name", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.connect("token=" + std::string(kGuestToken));
    CHECK(ws->closed_with() == mw::kWsCloseNameRequired);
    CHECK(app.add_guest_calls() == 0);
}

// from tests/test_meeting_ws.py::TestWsActionRoundTrip::test_guest_with_name_gets_hello
TEST_CASE("guest with name gets hello", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.guest("Sam");
    const Json hello = ws->receive();
    CHECK(hello["type"] == "hello");
    CHECK(hello["role"] == "guest");
    const std::string id = hello["participant_id"].get<std::string>();
    const Json participant = app.store->snapshot()["participants"][id];
    CHECK(participant["display_name"] == "Sam");
    CHECK(participant["kind"] == "guest");
}

// ---- TestGuestIdentity ----

// from tests/test_meeting_ws.py::TestGuestIdentity::test_reconnect_with_guest_id_reuses_participant
TEST_CASE("reconnect with guest id reuses participant", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    std::set<std::string> seen;
    for (int i = 0; i < 4; ++i) {
        auto ws = app.guest("Mallory", "g_stable1");
        seen.insert(ws->receive_until("hello")["participant_id"].get<std::string>());
        ws->close();
    }
    CHECK(seen.size() == 1);
    CHECK(app.add_guest_calls() == 1);
}

// from tests/test_meeting_ws.py::TestGuestIdentity::test_reconnect_without_guest_id_still_joins
TEST_CASE("reconnect without guest id still joins", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    for (int i = 0; i < 3; ++i) {
        auto ws = app.guest("Mallory");
        ws->receive_until("hello");
        ws->close();
    }
    CHECK(app.add_guest_calls() == 3);
}

// from tests/test_meeting_ws.py::TestGuestIdentity::test_changed_name_gets_a_new_participant
TEST_CASE("changed name gets a new participant", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    std::string first;
    std::string second;
    {
        auto ws = app.guest("Mallory", "g_stable1");
        first = ws->receive_until("hello")["participant_id"].get<std::string>();
        ws->close();
    }
    {
        auto ws = app.guest("Trudy", "g_stable1");
        second = ws->receive_until("hello")["participant_id"].get<std::string>();
        ws->close();
    }
    CHECK(first != second);
    CHECK(app.add_guest_calls() == 2);
}

// ---- TestGuestWsAuthorization ----

// from tests/test_meeting_ws.py::TestGuestWsAuthorization::test_guest_cannot_set_title
TEST_CASE("guest cannot set title", "[test_meeting_ws]") {
    Harness app(EngineKind::Store);
    const Json result = guest_action(app, {{"op", "set_title"}, {"text", "Guest rename"}});
    CHECK(result["results"][0]["ok"] == false);
    CHECK(result["results"][0]["reason"] == "host_only");
    CHECK(app.store->snapshot()["title"] == "");
}

// from tests/test_meeting_ws.py::TestGuestWsAuthorization::test_guest_cannot_toggle_cloud
TEST_CASE("guest cannot toggle cloud", "[test_meeting_ws]") {
    Harness app(EngineKind::Store);
    const Json result = guest_action(app, {{"op", "set_cloud_enabled"}, {"enabled", true}});
    CHECK(result["results"][0]["ok"] == false);
    CHECK(result["results"][0]["reason"] == "host_only");
    CHECK(app.store->snapshot()["cloud_enabled"] == false);
}

// from tests/test_meeting_ws.py::TestGuestWsAuthorization::test_guest_cannot_undo
TEST_CASE("guest cannot undo", "[test_meeting_ws]") {
    Harness app(EngineKind::Store);
    auto ws = app.guest("Mallory", "g_authz");
    ws->receive_until("hello");
    ws->send({{"type", "undo"}, {"client_action_id", "ca_u"}, {"seq", 1}});
    const Json result = ws->receive_until("action_result");
    CHECK(result["results"][0]["ok"] == false);
    CHECK(result["results"][0]["reason"] == "host_only");
    CHECK(app.engine->calls("undo").empty());
}

// from tests/test_meeting_ws.py::TestGuestWsAuthorization::test_guest_can_still_add_a_note
TEST_CASE("guest can still add a note", "[test_meeting_ws]") {
    // Control: the authorization tests above are not passing vacuously.
    Harness app(EngineKind::Store);
    const Json result =
        guest_action(app, {{"op", "add_item"}, {"card", "user_notes"}, {"text", "Guest note"}});
    CHECK(result["results"][0]["ok"] == true);
    const Json notes = app.store->snapshot()["cards"]["user_notes"];
    REQUIRE(notes.size() == 1);
    CHECK(notes[0]["text"] == "Guest note");
}

// from tests/test_meeting_ws.py::TestGuestWsAuthorization::test_host_may_set_title
TEST_CASE("host may set title", "[test_meeting_ws]") {
    Harness app(EngineKind::Store);
    auto ws = app.host();
    ws->receive_until("hello");
    ws->send({{"type", "action"},
              {"client_action_id", "ca_t"},
              {"op", {{"op", "set_title"}, {"text", "Host title"}}}});
    const Json result = ws->receive_until("action_result");
    CHECK(result["results"][0]["ok"] == true);
    CHECK(app.store->snapshot()["title"] == "Host title");
}

// ---- TestHostileInput ----

// from
// tests/test_meeting_ws.py::TestHostileInput::test_binary_frame_is_rejected_and_socket_survives
TEST_CASE("binary frame is rejected and socket survives", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.guest("Mallory");
    ws->receive_until("hello");
    ws->send_binary(std::string("\x00\x01\x02not json", 11));
    CHECK(ws->receive_until("error")["code"] == "binary_unsupported");
    ws->send({{"type", "ping"}});
    CHECK(ws->receive_until("pong")["type"] == "pong");
}

// from tests/test_meeting_ws.py::TestHostileInput::test_deeply_nested_json_is_rejected
TEST_CASE("deeply nested json is rejected", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.guest("Mallory");
    ws->receive_until("hello");
    ws->send_text(std::string(20000, '['));
    CHECK(ws->receive_until("error")["code"] == "bad_json");
    ws->send({{"type", "ping"}});
    CHECK(ws->receive_until("pong")["type"] == "pong");
}

// from tests/test_meeting_ws.py::TestHostileInput::test_oversize_payload_is_rejected_before_parsing
TEST_CASE("oversize payload is rejected before parsing", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.guest("Mallory");
    ws->receive_until("hello");
    ws->send_text(std::string(mw::kMaxMessageChars + 1, 'x'));
    CHECK(ws->receive_until("error")["code"] == "message_too_large");
    ws->send({{"type", "ping"}});
    CHECK(ws->receive_until("pong")["type"] == "pong");
}

// from tests/test_meeting_ws.py::TestHostileInput::test_absurd_undo_seq_is_rejected
TEST_CASE("absurd undo seq is rejected", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.host();
    ws->receive_until("hello");
    // 1e400 parses to float infinity; int() of it raises OverflowError.
    ws->send_text(R"({"type": "undo", "client_action_id": "ca_x", "seq": 1e400})");
    CHECK(ws->receive_until("action_result")["results"][0]["reason"] == "invalid_seq");
    ws->send_text(
        R"({"type": "undo", "client_action_id": "ca_y", "seq": 10000000000000000000000000000000000000000})");
    CHECK(ws->receive_until("action_result")["results"][0]["reason"] == "invalid_seq");
    ws->send({{"type", "ping"}});
    CHECK(ws->receive_until("pong")["type"] == "pong");
    CHECK(app.engine->calls("undo").empty());
}

// from tests/test_meeting_ws.py::TestHostileInput::test_non_object_and_malformed_op_are_rejected
TEST_CASE("non object and malformed op are rejected", "[test_meeting_ws]") {
    Harness app(EngineKind::Canned);
    auto ws = app.host();
    ws->receive_until("hello");
    ws->send_text("[1, 2, 3]");
    CHECK(ws->receive_until("error")["code"] == "bad_message");
    ws->send({{"type", "action"}, {"client_action_id", "ca_m"}, {"op", "not-a-dict"}});
    CHECK(ws->receive_until("action_result")["results"][0]["reason"] == "malformed_op");
    ws->send({{"type", "nonsense"}});
    const Json error = ws->receive_until("error");
    CHECK(error["code"] == "unknown_type");
    CHECK(error["message"] == "Unknown message type: 'nonsense'");
}

// ---- tests/test_meeting_custom_report.py::TestGuestScoping (the fan-out) ----

namespace {

fv::meeting_state::OpResult note_result() {
    fv::FakeClock clock;
    fv::meeting_state::testing::FixedIds ids;
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_past";
    fv::meeting_state::MeetingStateStore store(std::move(state), nullptr, {}, clock, ids);
    auto results = store.apply(
        "host", std::nullopt,
        Json::array({Json{{"op", "add_item"}, {"card", "user_notes"}, {"text", "a note"}}}));
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].ok);
    return results[0];
}

// What request_custom_report gives (meeting/state/custom_reports.py), built by hand: the op's
// handler is not part of this library's dependencies. The golden vectors replay the store's own
// results (test_golden.cpp, _on_store_batch).
fv::meeting_state::OpResult report_result() {
    fv::meeting_state::OpResult result;
    result.ok = true;
    result.op = {{"op", "request_custom_report"}, {"request", "a brief"}, {"run_id", "run_1"}};
    result.target_id = "rep_000000000002";
    result.effect = {{"entity", "custom_report"},
                     {"report", {{"id", "rep_000000000002"}, {"status", "running"}}},
                     {"removed", false}};
    result.seq = 2;
    return result;
}

} // namespace

// from
// tests/test_meeting_custom_report.py::TestGuestScoping::test_the_fan_out_splits_host_only_effects_onto_their_own_patch
TEST_CASE("the fan out splits host only effects onto their own patch",
          "[test_meeting_custom_report]") {
    const auto sent = mw::split_store_batch(2, {note_result(), report_result()});
    std::vector<Json> shared;
    std::vector<Json> host_only;
    for (const auto& broadcast : sent) {
        (broadcast.host_only ? host_only : shared).push_back(broadcast.message);
    }
    REQUIRE(shared.size() == 1);
    REQUIRE(host_only.size() == 1);
    REQUIRE(shared[0]["results"].size() == 1);
    CHECK(shared[0]["results"][0]["op"]["op"] == "add_item");
    std::set<std::string> ops;
    for (const auto& row : host_only[0]["results"]) {
        ops.insert(row["op"]["op"].get<std::string>());
    }
    CHECK(ops == std::set<std::string>{"add_item", "request_custom_report"});
}

// from
// tests/test_meeting_custom_report.py::TestGuestScoping::test_a_batch_of_only_shared_effects_sends_one_patch
TEST_CASE("a batch of only shared effects sends one patch", "[test_meeting_custom_report]") {
    const auto sent = mw::split_store_batch(1, {note_result()});
    REQUIRE(sent.size() == 1);
    CHECK_FALSE(sent[0].host_only);
}

// from
// tests/test_meeting_custom_report.py::TestGuestScoping::test_a_batch_of_only_reports_never_reaches_guests
TEST_CASE("a batch of only reports never reaches guests", "[test_meeting_custom_report]") {
    const auto sent = mw::split_store_batch(1, {report_result()});
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].host_only);
}

TEST_CASE("report patches reach hosts only over the socket", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    auto host = app.host();
    host->receive_until("hello");
    auto guest = app.guest("Sam");
    guest->receive_until("presence");
    host->receive_until("presence");
    app.server->hub().on_store_batch(2, {note_result(), report_result()});
    const Json shared = host->receive();
    const Json full = host->receive();
    CHECK(shared["results"].size() == 1);
    CHECK(full["results"].size() == 2);
    const Json guest_patch = guest->receive();
    CHECK(guest_patch["results"].size() == 1);
    guest->send({{"type", "ping"}});
    CHECK(guest->receive()["type"] == "pong");
}

// ---- What the hub adds ----

TEST_CASE("an action's patch arrives before its result", "[ws_hub]") {
    Harness app(EngineKind::Store);
    auto host = app.host();
    host->receive_until("hello");
    auto guest = app.guest("Sam", "g_order");
    guest->receive_until("presence");
    host->receive_until("presence");
    guest->send({{"type", "action"},
                 {"client_action_id", "ca_1"},
                 {"op", {{"op", "add_item"}, {"card", "user_notes"}, {"text", "Ordered"}}}});
    CHECK(guest->receive()["type"] == "patch");
    CHECK(guest->receive()["type"] == "action_result");
    CHECK(host->receive()["type"] == "patch");
}

TEST_CASE("rotation closes every socket with 4401 after a short delay", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    auto host = app.host();
    host->receive_until("hello");
    auto guest = app.guest("Sam");
    guest->receive_until("presence");
    const auto started = std::chrono::steady_clock::now();
    app.server->invalidate_connections();
    CHECK(host->closed_with() == mw::kWsCloseUnauthorized);
    CHECK(std::chrono::steady_clock::now() - started >= 200ms);
    CHECK(guest->closed_with() == mw::kWsCloseUnauthorized);
    CHECK(app.server->hub().connection_count() == 0);
}

TEST_CASE("a resync closes every socket with 4409 at once", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    auto host = app.host();
    host->receive_until("hello");
    app.server->invalidate_connections(true);
    CHECK(host->closed_with() == mw::kWsCloseResync);
}

TEST_CASE("sockets past the cap are refused with 4429", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    std::vector<std::unique_ptr<Client>> sockets;
    for (std::size_t i = 0; i < mw::kMaxConnections; ++i) {
        sockets.push_back(app.host());
        sockets.back()->receive_until("hello");
    }
    auto refused = app.host();
    CHECK(refused->closed_with() == mw::kWsCloseTooMany);
    sockets.back()->close();
    sockets.pop_back();
    // The departed socket's slot frees once its close is handled.
    for (int attempt = 0; attempt < 50 && app.server->hub().connection_count() >= 32; ++attempt) {
        std::this_thread::sleep_for(20ms);
    }
    auto admitted = app.host();
    CHECK(admitted->receive()["type"] == "hello");
}

TEST_CASE("a bad token or a missing meeting is refused with 4401", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    CHECK(app.connect("token=nope")->closed_with() == mw::kWsCloseUnauthorized);
    CHECK(app.connect("")->closed_with() == mw::kWsCloseUnauthorized);
    app.engine->set_meeting_id("");
    CHECK(app.host()->closed_with() == mw::kWsCloseUnauthorized);
    app.engine->set_meeting_id("m_ws");
    app.engine->set_store(nullptr);
    CHECK(app.host()->closed_with() == mw::kWsCloseUnauthorized);
}

TEST_CASE("a repository failure ends the handshake with 1011", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    app.repo->fail_get_meeting(true);
    CHECK(app.host()->closed_with() == mw::kWsCloseServerError);
}

TEST_CASE("the hub follows a store created after it started", "[ws_hub]") {
    Harness app(EngineKind::Store);
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_ws";
    auto replacement = std::make_shared<fv::meeting_state::MeetingStateStore>(
        std::move(state), nullptr, fv::meeting_state::SegmentHandler{}, app.clock, app.ids);
    replacement->apply("system", std::nullopt,
                       Json::array({Json{{"op", "upsert_participant"},
                                         {"display_name", "Me"},
                                         {"kind", "me"},
                                         {"is_provisional", false}}}));
    app.engine->set_store(replacement);
    auto host = app.host();
    host->receive_until("hello");
    // The old store no longer reaches the sockets; the new one does.
    app.store->apply("host", std::nullopt,
                     Json::array({Json{{"op", "set_topic"}, {"text", "Old store"}}}));
    replacement->apply("host", std::nullopt,
                       Json::array({Json{{"op", "set_topic"}, {"text", "New store"}}}));
    const Json patch = host->receive();
    CHECK(patch["type"] == "patch");
    CHECK(patch["results"][0]["effect"]["topic"]["current"] == "New store");
}

TEST_CASE("a guest's departure is announced to the others", "[ws_hub]") {
    Harness app(EngineKind::Canned);
    auto host = app.host();
    host->receive_until("hello");
    auto guest = app.guest("Sam");
    const std::string id = guest->receive_until("hello")["participant_id"].get<std::string>();
    host->receive_until("presence");
    guest->close();
    const Json left = host->receive_until("presence");
    CHECK(left["event"] == "left");
    CHECK(left["participant"]["id"] == id);
    CHECK(left["participant"]["display_name"] == "Sam");
}

TEST_CASE("the hub's logs carry no token or capability link", "[ws_hub]") {
    ScopedLogCapture logs;
    {
        Harness app(EngineKind::Store);
        CHECK_FALSE(app.server->host_url().empty());
        CHECK_FALSE(app.server->guest_url().empty());
        auto host = app.host();
        host->receive_until("hello");
        auto guest = app.guest("Sam", "g_logs");
        guest->receive_until("presence");
        guest->send_text("{not json");
        guest->receive_until("error");
        guest->send({{"type", "action"}, {"op", {{"op", "add_item"}, {"card", "user_notes"}}}});
        guest->receive_until("action_result");
        app.connect("token=nope")->closed_with();
        app.repo->fail_get_meeting(true);
        (void)app.server->host_url();
        app.repo->fail_get_meeting(false);
        app.server->invalidate_connections();
        host->closed_with();
    }
    const auto lines = logs.capture->lines();
    CHECK_FALSE(lines.empty());
    for (const auto& line : lines) {
        INFO(line);
        CHECK(line.find(kHostToken) == std::string::npos);
        CHECK(line.find(kGuestToken) == std::string::npos);
        CHECK(line.find("/m/") == std::string::npos);
    }
}
