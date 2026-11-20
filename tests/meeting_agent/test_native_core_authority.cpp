// What a native pass's tool calls may do (test_meeting_sidecar.py's TestToolAuthority and
// TestToolBridgeThreading, test_opencode_sidecar.py's authority tests): each call acts with the
// authority of the pass that made it, a canceled, timed-out or stale pass writes nothing more,
// and the results that reach the scheduler are the host's real ones. The stub sidecar's tool
// requests become tool calls the loopback provider streams.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#include "meeting_agent/tool_policy.h"
#include "native_support.h"

namespace {

using namespace fv::meeting_agent;
using namespace fv::meeting_agent::native_test;
using fv::Json;
using fv::meeting_state::OpResult;

const Json kRevision = {{"op", "revise_segment_text"},
                        {"segment_id", "sg_1"},
                        {"text", "corrected"},
                        {"evidence", Json::array({"sg_1"})}};

CheckpointPayload polish_payload(std::string id) {
    return {.request_id = std::move(id),
            .new_segments = {{{"id", "sg_1"}, {"text", "draft"}}},
            .is_polish = true};
}

TEST_CASE("a pass's tool calls keep their own pass's authority and real results", "[native]") {
    const auto kind = GENERATE(std::string("pi"), std::string("opencode"));
    const auto pass = GENERATE(std::string(PASS_POLISH), std::string(PASS_NOTES));
    CAPTURE(kind, pass);
    const Json allowed = pass == PASS_POLISH ? kRevision
                                             : Json{{"op", "add_item"},
                                                    {"card", "live_notes"},
                                                    {"text", "note"},
                                                    {"evidence", Json::array({"sg_1"})}};
    const Json denied = set_topic("bad");
    Harness harness(
        {probe(), tools_turn({call("write", "patch_state", patch(Json::array({denied, allowed})))}),
         text_turn()},
        kind);
    CheckpointPayload payload = polish_payload("active");
    payload.is_polish = pass == PASS_POLISH;
    payload.is_notes = pass == PASS_NOTES;
    const auto result = harness.core->checkpoint(payload);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(harness.tools->batches().size() == 1);
    CHECK(harness.tools->batches()[0] == Json::array({allowed}));
    REQUIRE(result.op_results.size() == 2);
    CHECK_FALSE(result.op_results[0].ok);
    CHECK(result.op_results[0].reason == (pass == PASS_POLISH ? POLISH_ONLY : NOTES_ONLY));
    CHECK(result.op_results[1].ok);
    CHECK(result.op_results[1].op == allowed);
}

TEST_CASE("question operations reach the scheduler with the pass's results", "[native]") {
    const auto kind = GENERATE(std::string("pi"), std::string("opencode"));
    CAPTURE(kind);
    Harness harness(
        {probe(),
         tools_turn({call("ask", "ask_question",
                          {{"text", "Who owns this?"}, {"evidence", Json::array({"sg_1"})}})}),
         text_turn()},
        kind);
    const auto result = harness.core->checkpoint({.request_id = "active"});
    REQUIRE(result.ok);
    REQUIRE(result.op_results.size() == 1);
    CHECK(result.op_results[0].op["op"] == "ask_question");
    REQUIRE(harness.tools->question_calls().size() == 1);
    CHECK(harness.tools->question_calls()[0].text == "Who owns this?");
}

TEST_CASE("a timed-out pass keeps its real writes and revokes the tool calls after it",
          "[native]") {
    const auto kind = GENERATE(std::string("pi"), std::string("opencode"));
    CAPTURE(kind);
    fv::FakeClock clock;
    Harness harness({probe(),
                     tools_turn({call("one", "patch_state", patch(Json::array({kRevision}))),
                                 call("two", "patch_state", patch(Json::array({kRevision})))}),
                     text_turn()},
                    kind, &clock);
    harness.tools->on_apply([&](const Json& ops) {
        // The live polish budget is 180 s of wall time; this write outlasts it.
        clock.advance(181s);
        std::vector<OpResult> results;
        for (const auto& op : ops) {
            results.push_back({.ok = true, .op = op});
        }
        return results;
    });
    const auto result = harness.core->checkpoint(polish_payload("timeout"));
    CHECK_FALSE(result.ok);
    CHECK(result.error == "Agent pass timed out after 180s");
    REQUIRE_FALSE(result.op_results.empty());
    CHECK(result.op_results[0].ok);
    CHECK(result.op_results[0].op == kRevision);
    // The second call of the same response never reached the host, and no turn followed.
    CHECK(harness.tools->batches().size() == 1);
    CHECK(harness.provider.requests().size() == 2);
    CHECK(harness.core->is_healthy());
}

TEST_CASE("a cancel during a write keeps that write and revokes the calls after it", "[native]") {
    // sidecar.py revokes a canceled request's authority once any admitted write has finished.
    // Here the cancel lands from the host's thread while the first call's write is in flight.
    const auto kind = GENERATE(std::string("pi"), std::string("opencode"));
    CAPTURE(kind);
    Harness harness({probe(),
                     tools_turn({call("one", "patch_state", patch(Json::array({kRevision}))),
                                 call("two", "ask_question", {{"text", "Late?"}})}),
                     text_turn()},
                    kind);
    std::atomic<int> applied{0};
    harness.tools->on_apply([&](const Json& ops) {
        ++applied;
        harness.core->cancel();
        return std::vector<OpResult>{{.ok = true, .op = ops[0]}};
    });
    const auto result = harness.core->checkpoint(polish_payload("revoked"));
    CHECK_FALSE(result.ok);
    CHECK(result.error == "canceled");
    // The call after it was refused, and counted rejected as tools.ts counted a bridge failure
    // (sidecar.py's canceled reply carried that count).
    REQUIRE(result.op_results.size() == 2);
    CHECK(result.op_results[0].op == kRevision);
    CHECK_FALSE(result.op_results[1].ok);
    CHECK(result.op_results[1].reason == "rejected");
    CHECK(applied == 1);
    CHECK(harness.tools->question_calls().empty());
    CHECK(harness.provider.requests().size() == 2);
    // A new generation runs clean.
    CHECK(harness.core->checkpoint(polish_payload("next")).ok);
}

TEST_CASE("a failed tool call tells the model and counts its ops as rejected", "[native]") {
    // tools.ts: "Tool bridge error, no ops were applied: <message>" and every op of the batch
    // (at least one) tallied as rejected; a failed question call tallies one; sidecar.py turned
    // the tallies into placeholder results for the scheduler.
    Harness harness({probe(),
                     tools_turn({call("one", "patch_state",
                                      patch(Json::array({set_topic("A"), set_topic("B")}))),
                                 call("two", "search_context_files", {{"query", "plan"}})}),
                     text_turn()});
    harness.tools->on_apply(
        [](const Json&) -> std::vector<OpResult> { throw std::runtime_error("disk full"); });
    const auto result = harness.core->checkpoint({.request_id = "req-1"});
    REQUIRE(result.ok);
    REQUIRE(result.op_results.size() == 2);
    for (const auto& op : result.op_results) {
        CHECK_FALSE(op.ok);
        CHECK(op.op == Json{{"op", "sidecar_tool_call"}});
        CHECK(op.reason == "rejected");
    }
    const auto replay = harness.bodies()[2]["messages"];
    REQUIRE(replay.size() == 5);
    CHECK(replay[3]["content"] == "Tool bridge error, no ops were applied: Meeting tool failed.");
    // The host's own words never reach the model.
    CHECK(replay.dump().find("disk full") == std::string::npos);
}

TEST_CASE("a pass of a stale generation never reaches the model or the host", "[native]") {
    // The stale restart generation of sidecar.py survives as the cancel generation: a pass that
    // was waiting behind another when cancel() came belongs to the old generation.
    const auto kind = GENERATE(std::string("pi"), std::string("opencode"));
    CAPTURE(kind);
    Harness harness(
        {probe(), silent(),
         tools_turn({call("late", "patch_state", patch(Json::array({set_topic("late")})))}),
         text_turn()},
        kind);
    auto first = std::async(std::launch::async,
                            [&] { return harness.core->checkpoint({.request_id = "req-a"}); });
    CancelOnExit stopFirst{*harness.core};
    REQUIRE(await_requests(harness.provider, 2));
    auto second = std::async(std::launch::async,
                             [&] { return harness.core->checkpoint({.request_id = "req-b"}); });
    CancelOnExit stopSecond{*harness.core};
    // req-b is queued behind req-a, which is stuck in a silent stream.
    CHECK_FALSE(finishes(second, 200ms));
    harness.core->cancel();
    REQUIRE(finishes(first, 3000ms));
    REQUIRE(finishes(second, 3000ms));
    const auto a = first.get();
    const auto b = second.get();
    CHECK_FALSE(a.ok);
    CHECK(a.error == "canceled");
    CHECK_FALSE(b.ok);
    CHECK(b.error == "canceled");
    CHECK(b.op_results.empty());
    CHECK(harness.provider.requests().size() == 2);
    CHECK(harness.tools->batches().empty());
    // The next generation's pass runs and writes.
    const auto next = harness.core->checkpoint({.request_id = "req-c"});
    CHECK(next.ok);
    CHECK(harness.tools->batches().size() == 1);
}

TEST_CASE("tool calls run one at a time in the order the model made them", "[native]") {
    Harness harness({probe(),
                     tools_turn({call("a", "patch_state", patch(Json::array({set_topic("A")}))),
                                 call("b", "ask_question", {{"text", "Why?"}}),
                                 call("c", "patch_state", patch(Json::array({set_topic("C")})))}),
                     text_turn()});
    std::atomic<int> inside{0};
    std::atomic<bool> overlapped{false};
    harness.tools->on_apply([&](const Json& ops) {
        if (++inside > 1) {
            overlapped = true;
        }
        std::this_thread::sleep_for(50ms);
        --inside;
        return std::vector<OpResult>{{.ok = true, .op = ops[0]}};
    });
    const auto result = harness.core->checkpoint({.request_id = "req-1"});
    REQUIRE(result.ok);
    CHECK_FALSE(overlapped);
    REQUIRE(result.op_results.size() == 3);
    CHECK(result.op_results[0].op["text"] == "A");
    CHECK(result.op_results[1].op["op"] == "ask_question");
    CHECK(result.op_results[2].op["text"] == "C");
    // Each tool result went back in call order in the next turn.
    const auto replay = harness.bodies()[2]["messages"];
    REQUIRE(replay.size() == 6);
    CHECK(replay[3]["tool_call_id"] == "a");
    CHECK(replay[4]["tool_call_id"] == "b");
    CHECK(replay[5]["tool_call_id"] == "c");
}

TEST_CASE("a slow tool call is progress, not a stall", "[native]") {
    fv::FakeClock clock;
    Harness harness(
        {probe(), tools_turn({call("slow", "patch_state", patch(Json::array({set_topic("A")})))}),
         text_turn()},
        "pi", &clock);
    harness.tools->on_apply([&](const Json& ops) {
        // The store is busy for longer than the live stall budget (45 s).
        clock.advance(50s);
        return std::vector<OpResult>{{.ok = true, .op = ops[0]}};
    });
    const auto result = harness.core->checkpoint({.request_id = "req-1"});
    INFO(result.error);
    CHECK(result.ok);
    CHECK(result.op_results.size() == 1);
}

} // namespace
