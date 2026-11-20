// The contracts' small pieces (activity records, timestamps, the installed agents' event map,
// usage sums) and the two fakes later packages build on.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "base/testing/fake_clock.h"
#include "meeting_agent/interfaces.h"
#include "meeting_agent/progress_labels.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_agent/testing/fake_tool_host.h"
#include "meeting_agent/tool_policy.h"
#include "meeting_agent/usage.h"

using fv::Json;
using namespace fv::meeting_agent;
using namespace std::chrono_literals;

TEST_CASE("activity records and their timestamps", "[interfaces]") {
    const AgentActivity activity{"tool", "Updating the dashboard…", "patch_state", "cards",
                                 "2026-01-01T00:00:00+00:00"};
    CHECK(activity.to_json().dump() ==
          R"({"kind":"tool","label":"Updating the dashboard…","tool":"patch_state",)"
          R"("pass_kind":"cards","ts":"2026-01-01T00:00:00+00:00"})");
    for (const auto kind : ACTIVITY_KINDS) {
        CHECK(is_activity_kind(kind));
    }
    CHECK_FALSE(is_activity_kind("message_update"));

    fv::FakeClock clock;
    CHECK(utc_now_iso(clock) == "2026-01-01T00:00:00+00:00");
    clock.advance(1500ms);
    CHECK(utc_now_iso(clock) == "2026-01-01T00:00:01.500000+00:00");
    clock.advance(std::chrono::microseconds(7));
    CHECK(utc_now_iso(clock) == "2026-01-01T00:00:01.500007+00:00");
}

TEST_CASE("installed agents' event kinds map to the shared progress copy", "[progress]") {
    const auto writing = pi_event("writing");
    REQUIRE(writing);
    CHECK(writing->first == "message_update");
    CHECK(writing->second == "text_delta");
    CHECK(progress_detail(writing->first, writing->second, PASS_NOTES) ==
          "Model is writing the meeting notes…");
    const auto settled = pi_event("settled");
    REQUIRE(settled);
    CHECK(activity_kind(settled->first, settled->second) == "settled");
    CHECK(progress_detail(settled->first, settled->second, PASS_POLISH) ==
          "Transcript cleanup is wrapping up…");
    for (const char* kind : {"start", "thinking", "writing", "tool", "turn", "retry", "settled"}) {
        const auto event = pi_event(kind);
        REQUIRE(event);
        CHECK(activity_kind(event->first, event->second) == kind);
    }
    CHECK_FALSE(pi_event("compaction"));
    CHECK_FALSE(pi_event(""));
}

TEST_CASE("merge_usage sums integer token counts and counts requests", "[usage]") {
    Json total = Json::object();
    merge_usage(total, {{"prompt_tokens", 10}, {"completion_tokens", 5}, {"total_tokens", 15}});
    merge_usage(total, nullptr);
    merge_usage(total, {{"prompt_tokens", 2.5}, {"completion_tokens", 1}});
    CHECK(total == Json{{"prompt_tokens", 10},
                        {"completion_tokens", 6},
                        {"total_tokens", 15},
                        {"requests", 2}});
}

TEST_CASE("FakeToolHost records ops, questions and searches", "[fakes]") {
    testing::FakeToolHost host;
    CHECK_FALSE(host.can_check_segments());
    CHECK_FALSE(host.can_search_past_meetings());
    CHECK_FALSE(host.can_search_context_files());
    host.set_known_segments({"sg_1"});
    CHECK(host.can_check_segments());
    CHECK(host.segment_exists("sg_1"));
    CHECK_FALSE(host.segment_exists("sg_2"));
    CHECK(host.segment_checks() == std::vector<std::string>{"sg_1", "sg_2"});

    const auto results = host.apply_agent_ops(Json::array({{{"op", "set_topic"}}}));
    REQUIRE(results.size() == 1);
    CHECK(results[0].ok);
    CHECK(results[0].op == Json{{"op", "set_topic"}});
    CHECK(host.batches().size() == 1);

    host.on_apply([](const Json& ops) {
        std::vector<fv::meeting_state::OpResult> out(ops.size());
        for (auto& result : out) {
            result.reason = "custom";
        }
        return out;
    });
    CHECK(host.apply_agent_ops(Json::array({Json::object()}))[0].reason == "custom");
    CHECK(host.ops().size() == 2);

    host.resolve_question("q_1", "Yes", 0.9, Json::array({"sg_1"}));
    const auto questions = host.question_calls();
    REQUIRE(questions.size() == 1);
    CHECK(questions[0].tool == "resolve_question");
    CHECK(questions[0].question_id == "q_1");
    CHECK(questions[0].text == "Yes");
    CHECK(questions[0].confidence == 0.9);

    host.set_context_files_answer("folder text");
    CHECK(host.search_context_files("q", "", 10) == "folder text");
    REQUIRE(host.search_calls().size() == 1);
    CHECK(host.search_calls()[0].tool == "search_context_files");
}

TEST_CASE("FakeAgentCore scripts results per pass and records payloads", "[fakes]") {
    testing::FakeAgentCore core;
    auto host = std::make_shared<testing::FakeToolHost>();
    AgentConfig config;
    config.meeting_id = "m1";
    core.initialize(config, host);
    CHECK(core.initialize_calls() == 1);
    REQUIRE(core.config());
    CHECK(core.config()->meeting_id == "m1");
    CHECK(core.tools() == host);

    AgentResult failed;
    failed.error = "boom";
    core.script(PASS_NOTES, failed);
    CheckpointPayload notes;
    notes.request_id = "r1";
    notes.is_notes = true;
    CHECK(core.checkpoint(notes).error == "boom");
    CHECK(core.checkpoint(notes).ok);
    CheckpointPayload final_pass;
    final_pass.is_consolidation = true;
    CHECK(core.consolidate(final_pass).ok);
    const auto passes = core.passes();
    REQUIRE(passes.size() == 3);
    CHECK(passes[0].kind == "notes");
    CHECK(passes[0].payload.request_id == "r1");
    CHECK(passes[2].kind == "consolidation");
    CHECK(passes[2].via_consolidate);

    core.set_hook([](const CheckpointPayload&, AgentToolHost* tools) {
        AgentResult result;
        result.ok = true;
        result.op_results = tools->apply_agent_ops(Json::array({{{"op", "set_topic"}}}));
        return result;
    });
    CHECK(core.checkpoint(CheckpointPayload{}).op_results.size() == 1);
    CHECK(host->ops().size() == 1);

    core.set_healthy(false);
    CHECK_FALSE(core.is_healthy());
    CHECK_FALSE(core.cadence());
    core.set_cadence(CheckpointCadence{45, 30, 90, 4, 180, 2, 120});
    REQUIRE(core.cadence());
    CHECK(core.cadence()->notes_min_s == 120);
    core.shutdown();
    CHECK(core.shutdown_calls() == 1);

    std::string progress;
    core.set_progress_callback([&progress](const std::string& text) { progress = text; });
    core.emit_progress("Reviewing the last few minutes…");
    CHECK(progress == "Reviewing the last few minutes…");

    core.fail_initialize("Pick an agent.");
    std::string message;
    try {
        core.initialize(config, host);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    CHECK(message == "Pick an agent.");
}

TEST_CASE("FakeAgentCore's hook keeps its tools while a new meeting replaces them", "[fakes]") {
    testing::FakeAgentCore core;
    auto first = std::make_shared<testing::FakeToolHost>();
    const std::weak_ptr<testing::FakeToolHost> watched = first;
    core.initialize(AgentConfig{}, std::move(first));
    core.set_hook([&core, &watched](const CheckpointPayload&, AgentToolHost* tools) {
        // Another thread starts the next meeting while this pass is still applying ops.
        core.initialize(AgentConfig{}, std::make_shared<testing::FakeToolHost>());
        CHECK_FALSE(watched.expired());
        AgentResult result;
        result.ok = true;
        result.op_results = tools->apply_agent_ops(Json::array({{{"op", "set_topic"}}}));
        return result;
    });
    const auto result = core.checkpoint(CheckpointPayload{});
    CHECK(result.op_results.size() == 1);
    // The pass let go of the first meeting's host once it ended.
    CHECK(watched.expired());
}

TEST_CASE("FakeAgentCore's delay ends early on cancel", "[fakes]") {
    testing::FakeAgentCore core;
    core.set_delay(10s);
    AgentResult result;
    std::thread pass([&] { result = core.checkpoint(CheckpointPayload{}); });
    REQUIRE(core.wait_for_passes(1, 5s));
    core.cancel();
    pass.join();
    CHECK_FALSE(result.ok);
    CHECK(result.error == "canceled");
    CHECK(core.cancel_calls() == 1);
}
