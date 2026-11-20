// The prompt tests of tests/test_meeting_notes_agent.py, test_meeting_intent.py,
// test_meeting_state.py and test_meeting_note_requests.py.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "base/identity.h"
#include "meeting_agent/prompts.h"
#include "meeting_agent/testing/fake_tool_host.h"
#include "meeting_agent/tool_policy.h"
#include "meeting_state/schema.h"

using fv::Json;
using namespace fv::meeting_agent;

namespace {

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::string lower(std::string text) {
    for (auto& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

// tests/fakes/scheduler.py segment: a two-second transcript row.
Json segment(const std::string& id, double start_s, const std::string& text = "hello there",
             const std::string& channel = "mic") {
    return {{"id", id},
            {"start_s", start_s},
            {"end_s", start_s + 2.0},
            {"text", text},
            {"channel", channel}};
}

Json empty_state() {
    return {
        {"participants", Json::object()}, {"cards", Json::object()}, {"questions", Json::array()}};
}

// The lines that open a numbered step ("1. ", "11. ").
std::vector<std::string> numbered_steps(const std::string& text) {
    static const std::regex step(R"(^(\d+)\. )");
    std::vector<std::string> steps;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        const std::string line =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (std::regex_search(line, step)) {
            steps.push_back(line);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return steps;
}

constexpr std::size_t kConsolidationSteps = 11;

const std::string kBrief = "We decide the vendor today. Capture who objected and why, and the "
                           "number attached to each bid.";

// tests/test_meeting_intent.py state_with_brief.
Json state_with_brief() {
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_intent";
    state.intent.text = kBrief;
    return state.to_json();
}

} // namespace

// from
// tests/test_meeting_notes_agent.py::TestNoteTakerPrompts::test_note_taker_system_prompt_persona_and_constraints
TEST_CASE("test_note_taker_system_prompt_persona_and_constraints", "[test_meeting_notes_agent]") {
    const auto prompt = build_note_taker_system_prompt();
    CHECK(has(lower(prompt), "note taker"));
    CHECK(has(prompt, "live_notes"));
    for (const char* op : {"add_item", "update_item", "remove_item"}) {
        CHECK(has(prompt, op));
    }
    CHECK(has(prompt, "data.heading"));
    CHECK(has(prompt, "data.start_s"));
    CHECK(has(prompt, "human_edited"));
    CHECK(has(prompt, "search_context_files"));
    CHECK(has(prompt, "untrusted"));
}

// from
// tests/test_meeting_notes_agent.py::TestNoteTakerPrompts::test_copilot_system_prompt_delegates_live_notes
TEST_CASE("test_copilot_system_prompt_delegates_live_notes", "[test_meeting_notes_agent]") {
    const auto prompt = build_system_prompt();
    CHECK(has(prompt, "live_notes"));
    CHECK(has(prompt, "note-taker pass"));
    CHECK(has(prompt, "search_past_meetings"));
    CHECK(has(prompt, "search_context_files"));
    CHECK(has(prompt, "CONTEXT ONLY"));
    CHECK(has(prompt, "untrusted"));
}

// from
// tests/test_meeting_notes_agent.py::TestNoteTakerPrompts::test_consolidation_instructions_finalize_notes
TEST_CASE("test_consolidation_instructions_finalize_notes", "[test_meeting_notes_agent]") {
    const auto prompt = build_checkpoint_user_prompt(empty_state(), {}, true);
    CHECK(has(prompt, "live_notes"));
    CHECK(has(prompt, "professional minutes"));
    // Reconciliation against the full recording, not just tidying: fix what later context
    // superseded, and rebuild an empty page.
    CHECK(has(prompt, "COMPLETE final transcript"));
    CHECK(has(prompt, "superseded"));
    CHECK(has(prompt, "notes page from the complete transcript"));
    CHECK((has(prompt, "actively taking into account the\nmeeting notes") ||
           has(prompt, "actively taking into account the meeting notes")));
    CHECK(has(prompt, "Synthesize the meeting notes and complete transcript"));
}

// from
// tests/test_meeting_notes_agent.py::TestNoteTakerPrompts::test_compact_state_renders_note_heading_and_extended_text
TEST_CASE("test_compact_state_renders_note_heading_and_extended_text",
          "[test_meeting_notes_agent]") {
    std::string long_note_text;
    for (int i = 0; i < 4; ++i) {
        long_note_text +=
            "This is a detailed and comprehensive note block explaining architectural decisions. ";
    }
    const Json state = {
        {"title", "Architecture Sync"},
        {"topic", {{"current", "Design Review"}}},
        {"rolling_summary", "Discussed system components."},
        {"cards",
         {{"live_notes",
           Json::array({{{"id", "it_n1"},
                         {"revision", 1},
                         {"status", "proposed"},
                         {"text", long_note_text},
                         {"data", {{"heading", "Architecture Review"}, {"start_s", 15.0}}}}})},
          {"key_points", Json::array({{{"id", "it_k1"},
                                       {"revision", 1},
                                       {"status", "proposed"},
                                       {"text", "Short key point."}}})}}},
        {"participants", Json::object()}};
    const auto rendered = render_state_compact(state);
    CHECK(has(rendered, "heading=Architecture Review"));
    CHECK(has(rendered, "start_s=15.0"));
    CHECK(has(rendered, "detailed and comprehensive note block"));
    // Not clipped at 240 characters.
    REQUIRE(long_note_text.size() > 300);
    CHECK(has(rendered, long_note_text.substr(0, 280)));
}

// from
// tests/test_meeting_notes_agent.py::TestNoteTakerPrompts::test_notes_user_prompt_renders_blocks_and_segments
TEST_CASE("test_notes_user_prompt_renders_blocks_and_segments", "[test_meeting_notes_agent]") {
    const Json state = {
        {"topic", {{"current", "Funnel review"}}},
        {"rolling_summary", "Team reviewed onboarding."},
        {"cards",
         {{"live_notes",
           Json::array({{{"id", "it_note1"},
                         {"revision", 2},
                         {"status", "proposed"},
                         {"text", "Maria walked through the funnel numbers."},
                         {"data", {{"heading", "Funnel review"}, {"start_s", 41.0}}}}})}}},
        {"participants", Json::object()}};
    const auto prompt =
        build_notes_user_prompt(state, {segment("sg_new", 95.0, "We agreed to try OAuth.")});
    CHECK(has(prompt, "## CURRENT NOTES PAGE"));
    CHECK(has(prompt, "it_note1"));
    CHECK(has(prompt, "rev=2"));
    CHECK(has(prompt, "heading=Funnel review"));
    CHECK(has(prompt, "start_s=41.0"));
    CHECK(has(prompt, "[sg_new]"));
    CHECK(has(prompt, "[t=95.0s]"));
    CHECK(has(prompt, "NOTE-TAKER PASS"));
}

// from
// tests/test_meeting_notes_agent.py::TestNoteTakerPrompts::test_notes_user_prompt_empty_page_hint
TEST_CASE("test_notes_user_prompt_empty_page_hint", "[test_meeting_notes_agent]") {
    const auto prompt =
        build_notes_user_prompt({{"cards", Json::object()}, {"participants", Json::object()}}, {});
    CHECK(has(prompt, "page is empty"));
    CHECK(has(prompt, "(no new segments)"));
}

// from
// tests/test_meeting_notes_agent.py::TestNotesToolScope::test_notes_scope_rejects_everything_but_live_notes_ops
TEST_CASE("test_notes_scope_rejects_everything_but_live_notes_ops", "[test_meeting_notes_agent]") {
    testing::FakeToolHost tools;
    ToolScope scope;
    scope.pass_kind = std::string(PASS_NOTES);
    scope.note_ids = {"it_note1"};

    const Json ops = Json::array({
        {{"op", "add_item"}, {"card", "live_notes"}, {"text", "new block"}, {"evidence", {"sg_1"}}},
        {{"op", "add_item"},
         {"card", "key_points"},
         {"text", "must not apply"},
         {"evidence", {"sg_1"}}},
        {{"op", "update_item"},
         {"id", "it_note1"},
         {"base_revision", 1},
         {"set", {{"text", "extend"}}},
         {"evidence", {"sg_1"}}},
        {{"op", "update_item"},
         {"id", "it_keypoint"},
         {"base_revision", 1},
         {"set", {{"text", "must not apply"}}},
         {"evidence", {"sg_1"}}},
        {{"op", "set_topic"}, {"text", "must not apply"}, {"evidence", {"sg_1"}}},
    });
    const auto [payload, results] = run_tool(tools, "patch_state", {{"ops", ops}}, scope);
    const auto [question, question_results] = run_tool(
        tools, "ask_question", {{"text", "must not apply"}, {"evidence", {"sg_1"}}}, scope);

    const auto applied = tools.ops();
    REQUIRE(applied.size() == 2);
    CHECK(applied[0]["op"] == "add_item");
    CHECK(applied[1]["op"] == "update_item");
    CHECK(applied[0]["card"] == "live_notes");
    REQUIRE(results.size() == 5);
    const std::vector<std::optional<std::string>> reasons = {
        std::nullopt, "notes_only", std::nullopt, "notes_only", "notes_only"};
    for (std::size_t i = 0; i < reasons.size(); ++i) {
        CHECK(results[i].reason == reasons[i]);
    }
    CHECK(question["reason"] == "notes_only");
    REQUIRE(question_results.size() == 1);
    CHECK_FALSE(question_results[0].ok);
}

// from tests/test_meeting_notes_agent.py::test_all_views_keep_every_step
TEST_CASE("test_all_views_keep_every_step", "[test_meeting_notes_agent]") {
    const auto text = build_consolidation_instructions(Json::array({"ribbon", "brief", "signal"}));
    CHECK(has(text, "Populate the timeline card"));
    CHECK(has(text, "professional minutes"));
    CHECK(has(text, "Make decisions and action items complete"));
    const auto steps = numbered_steps(text);
    CHECK(steps.size() == kConsolidationSteps);
    REQUIRE_FALSE(steps.empty());
    CHECK(steps[0].starts_with("1. "));
    CHECK(std::ranges::any_of(steps,
                              [](const std::string& line) { return line.starts_with("11. "); }));
}

// from tests/test_meeting_notes_agent.py::test_brief_only_omits_ribbon_steps
TEST_CASE("test_brief_only_omits_ribbon_steps", "[test_meeting_notes_agent]") {
    const auto text = build_consolidation_instructions(Json::array({"brief"}));
    CHECK_FALSE(has(text, "Populate the timeline card"));
    CHECK_FALSE(has(text, "professional minutes"));
    CHECK_FALSE(has(text, "notes page from the complete transcript"));
    CHECK(has(text, "Make decisions and action items complete"));
    CHECK(has(text, "Capture risks, blockers"));
    const auto steps = numbered_steps(text);
    CHECK(steps.size() == kConsolidationSteps - 2);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        CHECK(steps[i].starts_with(std::to_string(i + 1) + "."));
    }
}

// from tests/test_meeting_notes_agent.py::test_empty_views_keep_all_steps
TEST_CASE("test_empty_views_keep_all_steps", "[test_meeting_notes_agent]") {
    const auto text = build_consolidation_instructions(Json::array());
    CHECK(has(text, "Populate the timeline card"));
    CHECK(has(text, "professional minutes"));
}

// from tests/test_meeting_notes_agent.py::test_checkpoint_prompt_reads_report_views
TEST_CASE("test_checkpoint_prompt_reads_report_views", "[test_meeting_notes_agent]") {
    Json state = empty_state();
    state["report_views"] = Json::array({"brief"});
    const auto prompt = build_checkpoint_user_prompt(state, {}, true);
    CHECK_FALSE(has(prompt, "Populate the timeline card"));
    CHECK_FALSE(has(prompt, "professional minutes"));
    CHECK(has(prompt, "Make decisions and action items complete"));
}

// from tests/test_meeting_notes_agent.py::test_state_render_lists_open_questions_oldest_first
TEST_CASE("test_state_render_lists_open_questions_oldest_first", "[test_meeting_notes_agent]") {
    const auto rendered = render_state_compact(
        {{"questions",
          Json::array(
              {{{"id", "q_2"}, {"text", "Who owns QA?"}, {"status", "open"}, {"asked_at", "2"}},
               {{"id", "q_1"},
                {"text", "Budget?"},
                {"status", "open"},
                {"asked_at", "1"},
                {"suggested_answer", "$500"},
                {"suggested_confidence", 0.5}},
               {{"id", "q_3"}, {"text", "Settled"}, {"status", "resolved"}, {"asked_at", "0"}}})}});
    const auto at = rendered.find("Open questions (2/7):");
    REQUIRE(at != std::string::npos);
    const auto block = rendered.substr(at);
    CHECK(block.find("[q_1]") < block.find("[q_2]"));
    CHECK(has(block, "(confidence 0.50): $500"));
    CHECK_FALSE(has(block, "q_3"));
    CHECK(has(block, "You may open 5 more question(s)."));
    CHECK(has(block, "confidence >= 0.8"));
    CHECK(render_state_compact(Json::object()).ends_with("over asking new ones."));
    CHECK(has(render_state_compact(Json::object()), "Open questions: none."));
}

// from tests/test_meeting_intent.py::TestIntentPrompt::test_no_brief_adds_nothing_to_the_prompt
TEST_CASE("test_no_brief_adds_nothing_to_the_prompt", "[test_meeting_intent]") {
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m";
    CHECK(intent_prompt(state.to_json()).empty());
}

// from
// tests/test_meeting_intent.py::TestIntentPrompt::test_the_block_carries_the_brief_and_its_guardrails
TEST_CASE("test_the_block_carries_the_brief_and_its_guardrails", "[test_meeting_intent]") {
    const auto block = intent_prompt(state_with_brief());
    CHECK(has(block, kBrief));
    CHECK(has(block, "MEETING BRIEF"));
    // The brief says what to look for; it must never become a citation or license an insight
    // the meeting has not actually produced.
    CHECK(has(block, "never evidence"));
    CHECK(has(block, "never invent it"));
    CHECK(has(block, "does not change your tools"));
}

// from tests/test_meeting_intent.py::TestIntentPrompt::test_both_agent_personas_receive_the_brief
TEST_CASE("test_both_agent_personas_receive_the_brief", "[test_meeting_intent]") {
    const Json state = state_with_brief();
    CHECK(has(build_checkpoint_user_prompt(state, {}), kBrief));
    CHECK(has(build_notes_user_prompt(state, {}), kBrief));
}

// from
// tests/test_meeting_intent.py::TestIntentPrompt::test_the_final_report_pass_still_works_to_the_brief
TEST_CASE("test_the_final_report_pass_still_works_to_the_brief", "[test_meeting_intent]") {
    const auto consolidation = build_checkpoint_user_prompt(state_with_brief(), {}, true);
    REQUIRE(has(consolidation, kBrief));
    // It frames the dashboard rather than trailing after it.
    CHECK(consolidation.find(kBrief) < consolidation.find("CURRENT DASHBOARD STATE"));
}

// from tests/test_meeting_state.py::test_spoken_note_is_marked_protected_in_the_agent_context
TEST_CASE("test_spoken_note_is_marked_protected_in_the_agent_context", "[test_meeting_state]") {
    const Json state = {{"cards",
                         {{"key_points", Json::array({{{"id", "spoken"},
                                                       {"text", "Budget A needs $1,000."},
                                                       {"author_type", "system"},
                                                       {"author_id", "voice_command"},
                                                       {"status", "proposed"}}})}}}};
    CHECK(has(render_state_compact(state), "author_type=system author_id=voice_command protected"));
}

// from tests/test_meeting_note_requests.py::test_prompt_includes_user_context_but_not_deleted_notes
TEST_CASE("test_prompt_includes_user_context_but_not_deleted_notes",
          "[test_meeting_note_requests]") {
    const auto prompt = build_notes_user_prompt(
        {{"cards",
          {{"user_notes", Json::array({{{"text", "Customer spells it Acme"}, {"status", "edited"}},
                                       {{"text", "deleted-secret"}, {"status", "removed"}}})}}}},
        {});
    CHECK(has(prompt, "Customer spells it Acme"));
    CHECK_FALSE(has(prompt, "deleted-secret"));
}

// from tests/test_meeting_note_requests.py::test_adjustment_replaces_automatic_append_instructions
TEST_CASE("test_adjustment_replaces_automatic_append_instructions",
          "[test_meeting_note_requests]") {
    const auto prompt = build_notes_user_prompt(
        {{"cards", Json::object()}, {"note_adjustment_request", "Use bullets"}}, {});
    CHECK(has(prompt, "Use bullets"));
    CHECK(has(prompt, "No new speech is required"));
    CHECK_FALSE(has(prompt, "Extend the notes page"));
}

TEST_CASE("the prompts name the product, never OpenWhisper's name", "[prompts]") {
    for (const auto& prompt : {build_system_prompt(), build_note_taker_system_prompt()}) {
        CHECK(has(prompt, std::string(fv::identity().product) + "'s Meeting Mode"));
        CHECK_FALSE(has(lower(prompt), "open"
                                       "whisper"));
    }
}
