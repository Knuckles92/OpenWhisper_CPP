// The native core's passes as the scheduler sees them (test_meeting_sidecar.py's handshake,
// checkpoint, notes and polish tests; test_meeting_live_agent_workflow.py's sidecar pass
// results; test_meeting_direct_deadline.py's deadlines, ported against the built-in loop).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "meeting_agent/agent_request.h"
#include "meeting_agent/native_tools.h"
#include "meeting_agent/prompts.h"
#include "meeting_agent/tool_specs.h"
#include "native_support.h"

namespace {

using namespace fv::meeting_agent;
using namespace fv::meeting_agent::native_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::meeting_state::OpResult;

const Json kNotesState = {
    {"meeting_id", "m_notes"},
    {"seq", 3},
    {"cards",
     {{"live_notes", Json::array({{{"id", "it_note9"},
                                   {"card", "live_notes"},
                                   {"text", "existing block"},
                                   {"data", {{"heading", "Funnel"}, {"start_s", 12.0}}},
                                   {"status", "proposed"},
                                   {"revision", 1},
                                   {"pinned", false}}})},
      {"key_points", Json::array({{{"id", "it_key1"},
                                   {"card", "key_points"},
                                   {"text", "a key point"},
                                   {"status", "proposed"},
                                   {"revision", 1},
                                   {"pinned", false}}})}}},
    {"participants", Json::object()}};

std::vector<Json> segments() {
    return {{{"id", "sg_1"}, {"start_s", 0.0}, {"text", "hello"}}};
}

TEST_CASE("the OpenCode core runs every meeting pass with fresh history", "[native]") {
    // test_python_supervisor_with_packaged_sdk_all_meeting_passes, without Bun or the SDK: four
    // passes, two model calls each, every pass's first call without tool messages, every op
    // applied.
    const std::vector<std::pair<CheckpointPayload, Json>> passes = {
        {{.request_id = "req_0"}, set_topic("Budget")},
        {{.request_id = "req_1", .is_notes = true},
         {{"op", "add_item"},
          {"card", "live_notes"},
          {"text", "Budget cap is $500."},
          {"data", {{"heading", "Budget"}, {"start_s", 1}}},
          {"evidence", Json::array({"sg_1"})}}},
        {{.request_id = "req_2", .is_polish = true},
         {{"op", "revise_segment_text"},
          {"segment_id", "sg_1"},
          {"text", "Budget is $500."},
          {"evidence", Json::array({"sg_1"})}}},
        {{.request_id = "req_3", .is_consolidation = true},
         {{"op", "set_rolling_summary"},
          {"text", "The budget cap is $500."},
          {"evidence", Json::array({"sg_1"})}}},
    };
    std::vector<SseReply> replies{probe()};
    for (const auto& [payload, op] : passes) {
        replies.push_back(tools_turn({call("call_test", "patch_state", patch(Json::array({op})))}));
        replies.push_back(text_turn());
    }
    Harness harness(std::move(replies), "opencode");
    for (auto [payload, op] : passes) {
        payload.new_segments = {{{"id", "sg_1"},
                                 {"start_s", 1},
                                 {"end_s", 4},
                                 {"text", "Budget is five hundred."},
                                 {"channel", "mic"}}};
        const auto result = harness.core->checkpoint(payload);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(result.op_results.size() == 1);
        CHECK(result.op_results[0].ok);
        CHECK(result.op_results[0].op["op"] == op["op"]);
    }
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 9);
    for (std::size_t i = 1; i < bodies.size(); i += 2) {
        CHECK(bodies[i]["messages"].size() == 2);
        CHECK(bodies[i]["messages"].dump().find("\"tool\"") == std::string::npos);
        // Exactly one system message, and only the five meeting tools.
        CHECK(bodies[i]["messages"][0]["role"] == "system");
        CHECK(bodies[i]["tools"] == native_tools());
    }
    CHECK(harness.core->is_healthy());
}

TEST_CASE("a meeting without an API key cannot start the built-in agent", "[native]") {
    SseProvider provider({});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto core = create_agent_core("pi", deps);
    AgentConfig config;
    config.meeting_id = "m_side";
    config.provider = "openrouter";
    config.model = "test-model";
    config.system_prompt = "You are a test agent.";
    CHECK_THROWS_WITH(core->initialize(config, std::make_shared<testing::FakeToolHost>()),
                      ContainsSubstring("API key") &&
                          ContainsSubstring("No openrouter API key found"));
    CHECK_FALSE(core->is_healthy());
    const auto refused = core->checkpoint({.request_id = "req-1"});
    CHECK_FALSE(refused.ok);
    CHECK(provider.requests().empty());
}

TEST_CASE("a custom endpoint needs no key and its fields carry no secret", "[native]") {
    // test_custom_endpoint_env_and_initialize_fields: the base URL and kind reach the loop's
    // requests; the process environment half is gone with the process.
    SseProvider provider({probe(), text_turn()});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto core = create_agent_core("pi", deps);
    auto config = local_config(provider);
    config.api_key.reset();
    core->initialize(config, std::make_shared<testing::FakeToolHost>());
    CHECK(core->is_healthy());
    const auto fields = endpoint_fields(config);
    CHECK(fields["base_url"] == provider.url("/v1"));
    CHECK(fields["kind"] == "custom");
    CHECK(fields["model_metadata"]["protocol"] == "chat");
    CHECK(fields["headers"] == Json::object());
    CHECK_FALSE(fields.contains("api_key"));
    REQUIRE(core->checkpoint({.request_id = "req-1"}).ok);
    const auto requests = provider.requests();
    REQUIRE(requests.size() == 2);
    for (const auto& request : requests) {
        CHECK(request.path == "/v1/chat/completions");
    }
}

TEST_CASE("the polish flag runs the polish prompt and returns the actual revision", "[native]") {
    const Json revision = {{"op", "revise_segment_text"},
                           {"segment_id", "sg_1"},
                           {"text", "Anthropic"},
                           {"evidence", Json::array({"sg_1"})}};
    Harness harness({probe(),
                     tools_turn({call("w", "patch_state",
                                      patch(Json::array({revision, set_topic("not polish")})))}),
                     text_turn()});
    const CheckpointPayload payload{
        .request_id = "req-polish", .new_segments = segments(), .is_polish = true};
    const auto result = harness.core->checkpoint(payload);
    REQUIRE(result.ok);
    REQUIRE(result.op_results.size() == 2);
    CHECK(result.op_results[0].op["op"] == "revise_segment_text");
    CHECK(std::ranges::count_if(result.op_results, [](const auto& op) { return op.ok; }) == 1);
    CHECK(std::ranges::count_if(result.op_results, [](const auto& op) { return !op.ok; }) == 1);
    const auto messages = harness.bodies()[1]["messages"];
    CHECK(messages[0]["content"] == "The meeting charter.");
    CHECK(messages[1]["content"] ==
          build_checkpoint_user_prompt(Json::object(), segments(), false, true));
}

TEST_CASE("the notes flag swaps in the note-taker persona; a plain pass keeps the charter",
          "[native]") {
    Harness harness({probe(), text_turn(), text_turn()});
    REQUIRE(harness.core
                ->checkpoint(
                    {.request_id = "req-notes", .state_snapshot = kNotesState, .is_notes = true})
                .ok);
    REQUIRE(
        harness.core->checkpoint({.request_id = "req-plain", .state_snapshot = kNotesState}).ok);
    const auto bodies = harness.bodies();
    const auto notes = bodies[1]["messages"];
    CHECK(notes[0]["content"] == build_note_taker_system_prompt());
    CHECK(notes[1]["content"] == build_notes_user_prompt(kNotesState, {}));
    const auto plain = bodies[2]["messages"];
    CHECK(plain[0]["content"] == "The meeting charter.");
    CHECK(plain[1]["content"] == build_checkpoint_user_prompt(kNotesState, {}, false, false));
}

TEST_CASE("a notes review keeps the revision op and the payload's state as given", "[native]") {
    // test_sidecar_retains_transcript_revision_operations_and_shared_prompt.
    const Json add = {{"op", "add_item"},
                      {"card", "live_notes"},
                      {"text", "Anthropic makes Claude."},
                      {"evidence", Json::array({"sg_1"})}};
    Harness harness(
        {probe(), tools_turn({call("w", "patch_state", patch(Json::array({add})))}), text_turn()});
    const CheckpointPayload payload{
        .request_id = "request",
        .state_snapshot = {{"cards", Json::object()}, {"notes_review_requested", true}},
        .is_notes = true};
    const auto result = harness.core->checkpoint(payload);
    REQUIRE(result.ok);
    REQUIRE(result.op_results.size() == 1);
    CHECK(result.op_results[0].op["op"] == "add_item");
    CHECK_FALSE(payload.state_snapshot.contains("note_adjustment_request"));
    const auto prompt = harness.bodies()[1]["messages"][1]["content"].get<std::string>();
    CHECK_THAT(prompt, ContainsSubstring("REVIEW EXISTING NOTES NOW"));
    CHECK_THAT(prompt, !ContainsSubstring("note_adjustment_request"));
}

TEST_CASE("the read tools work during polish and are never tallied as writes", "[native]") {
    const bool recall = GENERATE(true, false);
    CAPTURE(recall);
    const Json args = recall
                          ? Json{{"query", "budget"}, {"limit", 5}}
                          : Json{{"query", "roadmap"}, {"relative_path", "plan.md"}, {"limit", 5}};
    const std::string tool = recall ? "search_past_meetings" : "search_context_files";
    const std::string citation = recall ? "past:m_old:1" : "file:plan.md:1";
    Harness harness({probe(), tools_turn({call("s", tool, args)}), text_turn()});
    harness.tools->set_past_meetings_answer(
        {{"ok", true},
         {"text", "past:m_old:1  \"Prior\" (2026-01-01) t=1s — match"},
         {"hits", Json::array({{{"ref", "past:m_old:1"}, {"meeting_id", "m_old"}}})}});
    harness.tools->set_context_files_answer(
        {{"ok", true},
         {"text", "file:plan.md:1  plan.md — roadmap excerpt"},
         {"hits", Json::array({{{"ref", "file:plan.md:1"}, {"path", "plan.md"}}})}});
    const auto result = harness.core->checkpoint({.request_id = "req-polish",
                                                  .state_snapshot = {{"meeting_id", "m_live"}},
                                                  .is_polish = true});
    REQUIRE(result.ok);
    CHECK(result.op_results.empty());
    CHECK(harness.tools->ops().empty());
    const auto calls = harness.tools->search_calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].tool == tool);
    CHECK(calls[0].query == (recall ? "budget" : "roadmap"));
    CHECK(calls[0].target == (recall ? "" : "plan.md"));
    CHECK(calls[0].limit == 5);
    const auto answer = harness.bodies()[2]["messages"][3]["content"].get<std::string>();
    CHECK_THAT(answer, ContainsSubstring(citation));
}

TEST_CASE("cancel ends the running pass and every pass waiting behind it", "[native]") {
    Harness harness({probe(), silent(), text_turn()});
    auto running = std::async(std::launch::async,
                              [&] { return harness.core->checkpoint({.request_id = "r1"}); });
    CancelOnExit stopRunning{*harness.core};
    REQUIRE(await_requests(harness.provider, 2));
    auto queued = std::async(std::launch::async, [&] {
        return harness.core->consolidate({.request_id = "r2", .is_consolidation = true});
    });
    CancelOnExit stopQueued{*harness.core};
    CHECK_FALSE(finishes(queued, 100ms));
    harness.core->cancel();
    REQUIRE(finishes(running, 3000ms));
    REQUIRE(finishes(queued, 3000ms));
    CHECK(running.get().error == "canceled");
    CHECK(queued.get().error == "canceled");
    CHECK(harness.provider.requests().size() == 2);
    CHECK(harness.core->is_healthy());
}

// test_signature_recovery_is_bounded_and_preserves_edits: a polish pass that Gemini refused
// for a corrupted thought signature runs once more, with fresh history, inside what is left of
// its budget, unless it already wrote something, was canceled, ran out of time, or was not a
// polish pass.
const Json kRevise = {{"op", "revise_segment_text"},
                      {"segment_id", "sg_1"},
                      {"text", "Hello."},
                      {"evidence", Json::array({"sg_1"})}};

SseReply signature_rejected() {
    return SseReply::json(
        400, {{"error", {{"code", 400}, {"message", "Corrupted thought signature."}}}});
}

TEST_CASE("a corrupted thought signature retries a clean polish once", "[native]") {
    fv::FakeClock clock;
    Harness harness({probe(), signature_rejected(),
                     tools_turn({call("w", "patch_state", patch(Json::array({kRevise})))})},
                    "pi", &clock);
    std::atomic<int> starts{0};
    harness.core->set_activity_callback([&](const AgentActivity& tick) {
        // The first attempt takes 35 s of the 180 s polish budget.
        if (tick.kind == "start" && starts++ == 0) {
            clock.advance(35s);
        }
    });
    harness.tools->on_apply([&](const Json& ops) {
        // The retry outlasts the 145 s it has left.
        clock.advance(146s);
        return std::vector<OpResult>{{.ok = true, .op = ops[0]}};
    });
    const auto result = harness.core->checkpoint(
        {.request_id = "original", .new_segments = segments(), .is_polish = true});
    CHECK(starts == 2);
    CHECK_FALSE(result.ok);
    CHECK(result.error == "Agent pass timed out after 145s");
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 3);
    CHECK(bodies[2]["messages"] == bodies[1]["messages"]);
}

TEST_CASE("signature recovery is bounded", "[native]") {
    const auto scenario =
        GENERATE(std::string("recover"), std::string("repeat_error"), std::string("partial"),
                 std::string("other_error"), std::string("canceled"), std::string("unhealthy"),
                 std::string("rolling"), std::string("expired"));
    CAPTURE(scenario);
    fv::FakeClock clock;
    std::vector<SseReply> replies{probe()};
    if (scenario == "partial") {
        replies.push_back(tools_turn({call("w", "patch_state", patch(Json::array({kRevise})))}));
    }
    replies.push_back(scenario == "other_error"
                          ? SseReply::json(400, {{"error", {{"message", "Invalid argument."}}}})
                          : signature_rejected());
    replies.push_back(scenario == "repeat_error" ? signature_rejected() : text_turn());
    Harness harness(std::move(replies), "pi", &clock);
    harness.core->set_progress_callback([&](const std::string& line) {
        if (line != "Transcript cleanup is wrapping up…" && line != "Checkpoint is wrapping up…") {
            return;
        }
        if (scenario == "canceled") {
            harness.core->cancel();
        } else if (scenario == "unhealthy") {
            harness.core->shutdown();
        } else if (scenario == "expired") {
            clock.advance(181s);
        }
    });
    const auto result = harness.core->checkpoint(
        {.request_id = "original", .new_segments = segments(), .is_polish = scenario != "rolling"});
    const std::size_t attempts = scenario == "recover" || scenario == "repeat_error" ? 2 : 1;
    const std::size_t turns = scenario == "partial" ? 2 : 1;
    CHECK(harness.provider.requests().size() == 1 + turns + attempts - 1);
    CHECK(result.ok == (scenario == "recover"));
    // ARCHITECTURE.md's provider error with the provider's cleaned message (M10.22); the
    // signature sentence is what the polish retry finds in it.
    if (scenario == "repeat_error") {
        CHECK(result.error ==
              "Local test model turn failed (HTTP 400): Corrupted thought signature.");
    }
    if (scenario == "other_error") {
        CHECK(result.error == "Local test model turn failed (HTTP 400): Invalid argument.");
    }
}

TEST_CASE("a cancel during a model request drops the reply's writes at once", "[native]") {
    // test_cancel_drops_late_writes_and_prevents_overlapping_requests: the canceled request is
    // aborted, not left running, so a late reply can neither write nor overlap the next pass.
    auto slow = tools_turn({call("late", "patch_state", patch(Json::array({set_topic("Late")})))});
    slow.piece_delay = 500ms;
    SseProvider provider({slow, text_turn()});
    const auto endpoint = local_profile(provider);
    AgentLoop loop(direct_client(), endpoint, "local-model",
                   fv::llm::model_spec(endpoint, "local-model"), {}, "dummy", fv::system_clock());
    std::atomic<int> executed{0};
    const ToolExecutor execute = [&](std::string_view, const Json&) {
        ++executed;
        return std::string("1 applied, 0 rejected");
    };
    fv::CancelSource cancel;
    auto pass = std::async(std::launch::async, [&] {
        return loop.run_pass("charter", "prompt", meeting_tools(), execute, {10, {}},
                             cancel.token());
    });
    REQUIRE(await_requests(provider, 1));
    const auto canceled_at = std::chrono::steady_clock::now();
    cancel.cancel();
    REQUIRE(finishes(pass, 2000ms));
    CHECK(std::chrono::steady_clock::now() - canceled_at < 1500ms);
    const auto outcome = pass.get();
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.canceled);
    CHECK(outcome.error == "canceled");
    // The reply never arrived, so no call of it was skipped either.
    CHECK(outcome.skipped_calls.empty());
    std::this_thread::sleep_for(1500ms);
    CHECK(executed == 0);
    CHECK(loop.run_pass("charter", "next", meeting_tools(), execute, {10, {}}, {}).ok);
    CHECK(executed == 0);
}

TEST_CASE("the capability probe has a wall deadline", "[native]") {
    fv::FakeClock clock;
    SseProvider provider({silent()});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    deps.clock = &clock;
    auto core = create_agent_core("pi", deps);
    auto starting = std::async(std::launch::async, [&] {
        core->initialize(local_config(provider), std::make_shared<testing::FakeToolHost>());
    });
    CancelOnExit stopStarting{*core};
    REQUIRE(await_requests(provider, 1));
    clock.advance(9s);
    CHECK_FALSE(finishes(starting, 200ms));
    clock.advance(2s);
    REQUIRE(finishes(starting, 3000ms));
    CHECK_THROWS_WITH(starting.get(), std::string(fv::llm::kStreamTimeoutError));
    CHECK_FALSE(core->is_healthy());
}

TEST_CASE("a trickling response ends at the pass budget before its tool call can run", "[native]") {
    // The provider keeps the connection busy with comment lines for four seconds and only then
    // sends the tool call; the pass's 0.2 s wall budget ends it long before.
    std::vector<std::string> pieces(100, ":\n\n");
    pieces.push_back(SseReply::frame(choice(
        {{"tool_calls",
          Json::array({call("late", "patch_state", patch(Json::array({set_topic("Late")})))})}})));
    pieces.push_back(SseReply::frame(choice(Json::object(), "tool_calls")));
    pieces.push_back("data: [DONE]\n\n");
    auto trickle = SseReply::stream(std::move(pieces));
    trickle.piece_delay = 40ms;
    SseProvider provider({trickle});
    const auto endpoint = local_profile(provider);
    AgentLoop loop(direct_client(), endpoint, "local-model",
                   fv::llm::model_spec(endpoint, "local-model"), {}, "dummy", fv::system_clock());
    std::atomic<int> executed{0};
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = loop.run_pass("sys", "user", meeting_tools(),
                                       [&](std::string_view, const Json&) {
                                           ++executed;
                                           return std::string();
                                       },
                                       {0.2, {}}, {});
    CHECK(std::chrono::steady_clock::now() - started < 2000ms);
    CHECK_FALSE(outcome.ok);
    CHECK_THAT(outcome.error, ContainsSubstring("timed out"));
    CHECK(executed == 0);
}

} // namespace
