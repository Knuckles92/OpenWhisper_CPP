// Audit AU18 (docs/port/audit/a27-meeting-sidecars-ts.json): what the built-in loop asks the
// model for on every pass (agent_request: Pi's reasoning and output-budget rules, M8.md change 4)
// and the tool texts it returns to the model (native_tool_result: tools.ts' summarize and
// result texts, A27-10, A27-11, A27-13), and runner.ts' 2 s throttle of the frequent deltas
// (A27-21).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "meeting_agent/agent_request.h"
#include "meeting_agent/native_tools.h"
#include "native_support.h"

namespace {

using fv::Json;
using fv::meeting_agent::agent_request;
using fv::meeting_agent::native_tool_result;
using fv::meeting_state::OpResult;

fv::llm::TextLLMProfile endpoint(std::string kind) {
    fv::llm::TextLLMProfile profile;
    profile.id = "audit";
    profile.name = "Audit";
    profile.kind = std::move(kind);
    profile.base_url = "http://127.0.0.1:9/v1";
    return profile;
}

fv::llm::TextModelSpec spec(bool reasoning, std::string format = "",
                            std::int64_t max_output = 4096) {
    fv::llm::TextModelSpec out;
    out.reasoning = reasoning;
    out.reasoning_format = std::move(format);
    out.max_output_tokens = max_output;
    return out;
}

std::string level_for(std::string kind, const fv::llm::TextModelSpec& model) {
    return agent_request(endpoint(std::move(kind)), "m", model, Json::array(), Json::array(), {})
        .reasoning_level;
}

TEST_CASE("a pass thinks at medium exactly when the model reasons", "[audit][native]") {
    // pi-adapter.ts registered `reasoning: metadata.reasoning ?? (kind is openrouter or
    // openai)`, but sidecar.py's model_metadata (spec.to_dict()) always carries a bool
    // reasoning, so Pi's kind default never applied: whatever the kind, Pi thought at medium
    // only for a reasoning spec.
    for (const auto* kind : {"custom", "openrouter", "openai"}) {
        CAPTURE(kind);
        CHECK(level_for(kind, spec(true)) == "medium");
        CHECK(level_for(kind, spec(false)).empty());
        CHECK(level_for(kind, spec(false, "deepseek")).empty());
        CHECK(level_for(kind, spec(true, "deepseek")) == "medium");
    }
}

TEST_CASE("a pass asks for the model's output budget, else Pi's 16,384 tokens", "[audit][native]") {
    const Json messages = Json::array(
        {{{"role", "system"}, {"content", "charter"}}, {{"role", "user"}, {"content", "prompt"}}});
    const Json tools = Json::array({{{"type", "function"}, {"function", {{"name", "noop"}}}}});
    fv::CancelSource cancel;
    const auto request = agent_request(endpoint("custom"), "local-model", spec(false, "", 8000),
                                       messages, tools, cancel.token());
    CHECK(request.model == "local-model");
    CHECK(request.messages == messages);
    REQUIRE(request.tools.has_value());
    CHECK(*request.tools == tools);
    CHECK(request.options["max_tokens"] == 8000);
    cancel.cancel();
    CHECK(request.cancel.cancelled());

    CHECK(agent_request(endpoint("custom"), "m", spec(false, "", 0), messages, tools, {})
              .options["max_tokens"] == 16384);
}

TEST_CASE("tool results read as tools.ts wrote them", "[audit][native]") {
    // patch_state: summarize() then the host's reply; a falsy ok counts as rejected.
    const Json reply = {{"results", Json::array({{{"ok", true}}, {{"ok", false}}})}};
    const std::vector<OpResult> ops{{.ok = true}, {.ok = false, .reason = std::string("x")}};
    CHECK(native_tool_result("patch_state", reply, ops) ==
          "1 applied, 1 rejected\n" + reply.dump());
    CHECK(native_tool_result("patch_state", {{"results", Json::array()}}, {}) ==
          "0 applied, 0 rejected\n{\"results\":[]}");

    // The question tools: the host's verdict as JSON.
    const Json verdict = {{"ok", false}, {"reason", "question_limit"}};
    CHECK(native_tool_result("ask_question", verdict, {{.ok = false}}) == verdict.dump());
    CHECK(native_tool_result("resolve_question", verdict, {{.ok = false}}) == verdict.dump());

    // The read tools: the answer's text when it has one, else the whole answer as JSON.
    CHECK(native_tool_result("search_past_meetings", {{"text", "Earlier plan"}, {"hits", 1}}, {}) ==
          "Earlier plan");
    const Json empty_text = {{"text", ""}, {"hits", Json::array()}};
    CHECK(native_tool_result("search_context_files", empty_text, {}) == empty_text.dump());
    const Json no_text = {{"hits", Json::array()}};
    CHECK(native_tool_result("search_context_files", no_text, {}) == no_text.dump());
}

TEST_CASE("frequent deltas are reported at most once per 2 s; every other event always",
          "[audit][native]") {
    // runner.ts emitProgress: thinking_delta, text_delta and toolcall_delta are dropped within
    // 2,000 ms of the last emitted event; lifecycle events always pass. The clock stands still,
    // so every delta of the turn falls within 2 s of agent_start and none is reported.
    using namespace fv::meeting_agent::native_test;
    SseProvider provider({thinking_turn(8)});
    const auto profile = local_profile(provider);
    fv::FakeClock clock;
    fv::meeting_agent::AgentLoop loop(direct_client(), profile, "local-model",
                                      fv::llm::model_spec(profile, "local-model"), {}, "dummy",
                                      clock);
    std::vector<fv::meeting_agent::LoopEvent> events;
    const auto outcome =
        loop.run_pass("charter", "prompt", fv::meeting_agent::meeting_tools(), {}, {60, 45}, {},
                      [&](const fv::meeting_agent::LoopEvent& event) { events.push_back(event); });
    INFO(outcome.error);
    REQUIRE(outcome.ok);
    const auto count = [&](std::string_view type, std::string_view delta = {}) {
        return std::ranges::count_if(
            events, [&](const auto& event) { return event.type == type && event.delta == delta; });
    };
    CHECK(count("message_update", "thinking_delta") == 0);
    CHECK(count("message_update", "text_delta") == 0);
    CHECK(count("message_update", "toolcall_delta") == 0);
    CHECK(count("agent_start") == 1);
    CHECK(count("turn_start") == 1);
    CHECK(count("turn_end") == 1);
    CHECK(count("agent_end") == 1);
    REQUIRE_FALSE(events.empty());
    CHECK(events.front().type == "agent_start");
    CHECK(events.back().type == "agent_end");
}

} // namespace
