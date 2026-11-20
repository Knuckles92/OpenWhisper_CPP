// tests/test_meeting_polish_agent.py's prompt and tool-policy tests, and the policy's other rules.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "meeting_agent/prompts.h"
#include "meeting_agent/testing/fake_tool_host.h"
#include "meeting_agent/tool_policy.h"
#include "pyport/errors.h"

using fv::Json;
using namespace fv::meeting_agent;

namespace {

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

ToolScope scope_for(std::string_view kind) {
    ToolScope scope;
    scope.pass_kind = std::string(kind);
    return scope;
}

} // namespace

// from tests/test_meeting_polish_agent.py::test_polish_prompt_limits_the_agent_to_transcript_text
TEST_CASE("test_polish_prompt_limits_the_agent_to_transcript_text", "[test_meeting_polish_agent]") {
    const auto prompt = build_checkpoint_user_prompt(
        {{"participants", Json::object()}, {"cards", Json::object()}, {"questions", Json::array()}},
        {Json{{"id", "sg_one"}, {"start_s", 1.0}, {"end_s", 2.0}, {"text", "helo world"}}}, false,
        true);
    CHECK(has(prompt, "TRANSCRIPT POLISH PASS"));
    CHECK(has(prompt, "ONLY revise_segment_text"));
    CHECK(has(prompt, "## TRANSCRIPT BLOCK FOR CLEANUP"));
    CHECK_FALSE(has(prompt, "## FULL MEETING TRANSCRIPT"));
    CHECK(has(prompt, "Review only this block"));
    CHECK(has(prompt, "search_past_meetings"));
    CHECK(has(prompt, "search_context_files"));
}

// from tests/test_meeting_polish_agent.py::test_polish_scope_rejects_state_and_question_tools
TEST_CASE("test_polish_scope_rejects_state_and_question_tools", "[test_meeting_polish_agent]") {
    testing::FakeToolHost tools;
    const auto scope = scope_for(PASS_POLISH);
    const auto [payload, results] =
        run_tool(tools, "patch_state",
                 {{"ops", Json::array({{{"op", "add_item"},
                                        {"card", "key_points"},
                                        {"text", "must not apply"},
                                        {"evidence", Json::array({"sg_one"})}},
                                       {{"op", "revise_segment_text"},
                                        {"segment_id", "sg_one"},
                                        {"text", "hello world"},
                                        {"evidence", Json::array({"sg_one"})}}})}},
                 scope);
    const auto [question_payload, question_results] =
        run_tool(tools, "ask_question",
                 {{"text", "must not apply"}, {"evidence", Json::array({"sg_one"})}}, scope);

    const auto applied = tools.ops();
    REQUIRE(applied.size() == 1);
    CHECK(applied[0]["op"] == "revise_segment_text");
    REQUIRE(results.size() == 2);
    CHECK_FALSE(results[0].ok);
    CHECK(results[0].reason == "polish_only");
    CHECK(results[1].ok);
    CHECK_FALSE(results[1].reason);
    CHECK(payload["results"][0]["reason"] == "polish_only");
    CHECK(payload["results"][1]["reason"].is_null());
    CHECK(question_payload["reason"] == "polish_only");
    REQUIRE(question_results.size() == 1);
    CHECK_FALSE(question_results[0].ok);
    CHECK(tools.question_calls().empty());
}

// from tests/test_meeting_polish_agent.py::test_missing_read_tool_says_so_in_plain_text
TEST_CASE("test_missing_read_tool_says_so_in_plain_text", "[test_meeting_polish_agent]") {
    testing::FakeToolHost tools;
    const auto [payload, results] =
        run_tool(tools, "search_past_meetings", {{"query", "budget"}}, ToolScope{});
    CHECK(payload["disabled"] == true);
    CHECK(results.empty());
    CHECK(tool_result_text("search_past_meetings", payload) ==
          "Past-meeting recall is not available.");
}

TEST_CASE("read tools run on every pass and are never tallied", "[tool_policy]") {
    testing::FakeToolHost tools;
    tools.set_past_meetings_answer(
        {{"ok", true}, {"text", "past:m_old:1 excerpt"}, {"hits", Json::array()}});
    tools.set_context_files_answer(
        {{"ok", true}, {"text", "file:plan.md:1 excerpt"}, {"hits", Json::array()}});
    const auto scope = scope_for(PASS_POLISH);
    const auto [content, content_results] =
        run_tool(tools, "search_past_meetings", {{"query", "budget"}}, scope);
    const auto [folder, folder_results] = run_tool(
        tools, "search_context_files", {{"query", "roadmap"}, {"relative_path", "plan.md"}}, scope);
    CHECK(tool_result_text("search_past_meetings", content) == "past:m_old:1 excerpt");
    CHECK(tool_result_text("search_context_files", folder) == "file:plan.md:1 excerpt");
    CHECK(content_results.empty());
    CHECK(folder_results.empty());
    CHECK(tools.ops().empty());
    const auto calls = tools.search_calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].query == "budget");
    CHECK(calls[0].target.empty());
    CHECK(calls[0].limit == 10);
    CHECK(calls[1].query == "roadmap");
    CHECK(calls[1].target == "plan.md");
    CHECK(calls[1].limit == 10);
}

TEST_CASE("a malformed batch or an unknown tool raises", "[tool_policy]") {
    testing::FakeToolHost tools;
    CHECK_THROWS_AS(run_tool(tools, "patch_state", {{"ops", "nope"}}, ToolScope{}),
                    fv::py::ValueError);
    CHECK_THROWS_AS(run_tool(tools, "noop", Json::object(), ToolScope{}), fv::py::ValueError);
    CHECK(tools.batches().empty());
    // run_read_tool looks the name up in _READ_TOOLS: a write tool is a KeyError, never a search.
    tools.set_context_files_answer({{"ok", true}, {"text", "hit"}, {"hits", Json::array()}});
    try {
        run_read_tool(tools, "patch_state", {{"query", "budget"}});
        FAIL("run_read_tool accepted a write tool");
    } catch (const fv::py::PyError& error) {
        CHECK(error.type_name() == "KeyError");
    }
    CHECK(tools.search_calls().empty());
}

TEST_CASE("scripted results come back in the order of the ops", "[tool_policy]") {
    testing::FakeToolHost tools;
    fv::meeting_state::OpResult mismatch;
    mismatch.ok = false;
    mismatch.reason = "revision_mismatch";
    mismatch.current_revision = 4;
    tools.script_results({mismatch});
    const auto scope = scope_for(PASS_NOTES);
    const auto results =
        apply_patch_ops(tools,
                        Json::array({{{"op", "set_topic"}, {"text", "x"}},
                                     {{"op", "add_item"}, {"card", "live_notes"}, {"text", "a"}},
                                     {{"op", "add_item"}, {"card", "live_notes"}, {"text", "b"}}}),
                        scope);
    REQUIRE(results.size() == 3);
    CHECK(results[0].reason == "notes_only");
    CHECK(results[1].reason == "revision_mismatch");
    CHECK(results[2].ok);
    CHECK(serialize_op_result(results[1]) == Json{{"ok", false},
                                                  {"reason", "revision_mismatch"},
                                                  {"target_id", nullptr},
                                                  {"seq", nullptr},
                                                  {"current_revision", 4}});
}

TEST_CASE("a payload's scope", "[tool_policy]") {
    CheckpointPayload payload;
    payload.is_notes = true;
    payload.state_snapshot = {{"cards", {{"live_notes", Json::array({{{"id", "it_n"}}})}}},
                              {"recent_transcript_context", Json::array({{{"id", "sg_old"}}})}};
    payload.new_segments = {Json{{"id", "sg_new"}}};
    const auto scope = ToolScope::for_payload(payload);
    CHECK(scope.pass_kind == "notes");
    CHECK(scope.note_ids == std::set<std::string, std::less<>>{"it_n"});
    CHECK(scope.citable_ids == std::vector<std::string>{"sg_new", "sg_old"});
    payload.is_polish = true;
    CHECK(pass_kind_for(payload) == "polish");
    payload.is_consolidation = true;
    CHECK(pass_kind_for(payload) == "consolidation");
    CHECK(ToolScope::for_payload(payload).note_ids.empty());
    CHECK(is_tool_name("search_context_files"));
    CHECK_FALSE(is_tool_name("noop"));
    CHECK(is_read_tool("search_past_meetings"));
    CHECK_FALSE(is_read_tool("patch_state"));
}
