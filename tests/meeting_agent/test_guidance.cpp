// tests/test_meeting_corrections.py's TestGuidancePrompt, and the two guidance_prompt assertions
// of tests/test_meeting_scoped_corrections.py that tests/meeting_state left out.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "base/testing/fake_clock.h"
#include "meeting_agent/guidance.h"
#include "meeting_agent/prompts.h"
#include "meeting_state/corrections.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"

using fv::Json;
using namespace fv::meeting_agent;

namespace {

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// tests/test_meeting_corrections.py _note.
Json note(const std::string& selected, const std::string& replacement,
          const std::string& kind = "term_correction", const std::string& text = "",
          const Json& overrides = Json::object()) {
    Json item = {
        {"id", "it_" + selected},
        {"card", "user_notes"},
        {"status", "edited"},
        {"author_type", "user"},
        {"text", text.empty() ? "Correction: " + selected + " -> " + replacement : text},
        {"data", {{"kind", kind}, {"selected_text", selected}, {"replacement", replacement}}}};
    for (const auto& [key, value] : overrides.items()) {
        item[key] = value;
    }
    return item;
}

Json state_of(const Json& notes) {
    return {{"cards", {{"user_notes", notes}}}};
}

} // namespace

// from
// tests/test_meeting_corrections.py::TestGuidancePrompt::test_includes_both_kinds_and_excludes_removed_or_agent_notes
TEST_CASE("test_includes_both_kinds_and_excludes_removed_or_agent_notes",
          "[test_meeting_corrections]") {
    const auto prompt = guidance_prompt(state_of(Json::array({
        note("Entropic", "Anthropic", "term_correction", "Correction: Entropic -> Anthropic"),
        note("plan", "", "agent_insight", "We mean the hiring plan"),
        note("secret", "Secret", "term_correction", "deleted-secret", {{"status", "removed"}}),
        note("agent", "Agent", "term_correction", "agent-authored", {{"author_type", "agent"}}),
        {{"text", "plain note"},
         {"status", "edited"},
         {"author_type", "user"},
         {"data", Json::object()}},
    })));
    CHECK(prompt.starts_with("## HUMAN MEETING GUIDANCE"));
    CHECK(has(prompt, "Correction: Entropic -> Anthropic"));
    CHECK(has(prompt, "We mean the hiring plan"));
    CHECK_FALSE(has(prompt, "deleted-secret"));
    CHECK_FALSE(has(prompt, "agent-authored"));
    CHECK_FALSE(has(prompt, "plain note"));
    CHECK(has(prompt, "do not invent"));
}

// from tests/test_meeting_corrections.py::TestGuidancePrompt::test_empty_without_guidance_notes
TEST_CASE("test_empty_without_guidance_notes", "[test_meeting_corrections]") {
    CHECK(guidance_prompt(Json::object()).empty());
    CHECK(
        guidance_prompt(state_of(Json::array(
                            {{{"text", "plain"}, {"status", "edited"}, {"author_type", "user"}}})))
            .empty());
}

// from tests/test_meeting_corrections.py::TestGuidancePrompt::test_agent_prompts_lead_with_guidance
TEST_CASE("test_agent_prompts_lead_with_guidance", "[test_meeting_corrections]") {
    Json state = state_of(Json::array({note("Entropic", "Anthropic")}));
    state["participants"] = Json::object();
    for (const auto& prompt :
         {build_checkpoint_user_prompt(state, {}), build_notes_user_prompt(state, {})}) {
        CHECK(prompt.starts_with("## HUMAN MEETING GUIDANCE"));
        CHECK(has(prompt, "\"replacement\": \"Anthropic\""));
    }
    CHECK_FALSE(build_checkpoint_user_prompt({{"participants", Json::object()}}, {})
                    .starts_with("## HUMAN MEETING GUIDANCE"));
}

// from
// tests/test_meeting_scoped_corrections.py::test_scoped_corrections_are_stable_persisted_exported_and_individually_undoable
// (its two guidance_prompt assertions; the store here has the evidence check and no repository,
// the persistence, export and undo halves run in tests/meeting_state)
TEST_CASE("scoped corrections reach the agent as cited, one-occurrence guidance",
          "[test_meeting_scoped_corrections]") {
    using namespace fv::meeting_state;
    const fv::FakeClock clock;
    fv::meeting_state::testing::FixedIds ids;
    const std::set<std::string, std::less<>> segments = {"sg_one", "sg_two"};
    SegmentPredicates predicates;
    predicates.segment_exists = [&segments](std::string_view id) {
        return segments.contains(id);
    };
    MeetingState state;
    state.meeting_id = "m_scoped";
    MeetingStateStore store(std::move(state), nullptr, {}, clock, ids, predicates);
    const auto scoped = [](int index, const std::string& replacement) {
        return Json{{"op", "add_item"},
                    {"card", "user_notes"},
                    {"text", "Correct occurrence " + std::to_string(index)},
                    {"evidence", Json::array({"sg_one"})},
                    {"data",
                     {{"kind", "occurrence_correction"},
                      {"selected_text", "word"},
                      {"replacement", replacement},
                      {"occurrence_index", index},
                      {"base_fingerprint", segment_fingerprint("word word word")}}}};
    };
    REQUIRE(store.apply("host", std::string("host"), Json::array({scoped(2, "third")}))[0].ok);
    REQUIRE(store.apply("host", std::string("host"), Json::array({scoped(0, "first")}))[0].ok);

    CHECK(term_rules(store.snapshot()).empty());
    const auto prompt = guidance_prompt(store.snapshot());
    CHECK(has(prompt, "\"evidence\": [\"sg_one\"]"));
    CHECK(has(prompt, "do not turn them into meeting-wide spelling rules"));
}

TEST_CASE("guidance keeps non-ASCII text as it is", "[guidance]") {
    const auto prompt =
        guidance_prompt(state_of(Json::array({{{"author_type", "user"},
                                               {"text", "Caf\xC3\xA9 \xE2\x9C\x93"},
                                               {"data", {{"kind", "agent_insight"}}},
                                               {"evidence", Json::array({"sg_1"})}}})));
    CHECK(has(prompt, "[{\"text\": \"Caf\xC3\xA9 \xE2\x9C\x93\", \"data\": {\"kind\": "
                      "\"agent_insight\"}, \"evidence\": [\"sg_1\"]}]"));
}
