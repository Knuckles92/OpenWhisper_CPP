// tests/test_meeting_spotlight_context.py: Top Insights selection, de-duplication and the
// rendering the agent reads.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "meeting_agent/prompts.h"
#include "meeting_agent/spotlight.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "meeting_state/testing/fixed_ids.h"

using fv::Json;
using namespace fv::meeting_agent;

namespace {

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::vector<std::string> ids_of(const std::vector<Json>& picks) {
    std::vector<std::string> ids;
    for (const auto& pick : picks) {
        ids.push_back(pick["id"].get<std::string>());
    }
    return ids;
}

bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::ranges::find(ids, id) != ids.end();
}

} // namespace

// from
// tests/test_meeting_spotlight_context.py::test_select_spotlight_items_ranking_and_deduplication
TEST_CASE("test_select_spotlight_items_ranking_and_deduplication",
          "[test_meeting_spotlight_context]") {
    const Json cards = {
        {"key_points", Json::array({{{"id", "it_1"},
                                     {"text", "Launch beta version on Monday"},
                                     {"status", "proposed"},
                                     {"pinned", false},
                                     {"updated_at", "2026-08-16T10:00:00Z"}},
                                    {{"id", "it_2"},
                                     {"text", "Different key takeaway regarding architecture"},
                                     {"status", "proposed"},
                                     {"pinned", false},
                                     {"updated_at", "2026-08-16T10:05:00Z"}}})},
        {"decisions", Json::array({{{"id", "it_3"},
                                    {"text", "Launch beta version by Monday morning"},
                                    {"status", "confirmed"},
                                    {"pinned", false},
                                    {"updated_at", "2026-08-16T10:02:00Z"}},
                                   {{"id", "it_4"},
                                    {"text", "Adopt SQLite for persistence"},
                                    {"status", "confirmed"},
                                    {"pinned", true},
                                    {"updated_at", "2026-08-16T10:01:00Z"}}})},
        {"action_items", Json::array({{{"id", "it_5"},
                                       {"text", "Deploy staging server"},
                                       {"status", "edited"},
                                       {"pinned", false},
                                       {"updated_at", "2026-08-16T10:03:00Z"}}})},
        {"live_notes", Json::array({{{"id", "it_note"},
                                     {"text", "Note taker block"},
                                     {"status", "proposed"},
                                     {"pinned", true},
                                     {"updated_at", "2026-08-16T10:09:00Z"}}})},
    };
    const auto picks = select_spotlight_items(cards, 3);
    REQUIRE(picks.size() == 3);
    // it_4 is pinned, so it ranks first.
    CHECK(picks[0]["id"] == "it_4");
    CHECK(picks[0]["card"] == "decisions");
    const auto ids = ids_of(picks);
    CHECK(contains(ids, "it_4"));
    // it_1 and it_3 paraphrase each other: at most one of them is featured.
    CHECK_FALSE((contains(ids, "it_1") && contains(ids, "it_3")));
}

// from tests/test_meeting_spotlight_context.py::test_render_state_compact_includes_top_insights
TEST_CASE("test_render_state_compact_includes_top_insights", "[test_meeting_spotlight_context]") {
    const Json state = {
        {"title", "Roadmap Sync"},
        {"topic", {{"current", "Q3 Planning"}}},
        {"rolling_summary", "Discussed roadmap and priorities."},
        {"participants",
         {{"p_me", {{"display_name", "Me"}, {"kind", "me"}, {"name_source", "human"}}}}},
        {"cards",
         {{"key_points", Json::array({{{"id", "it_1"},
                                       {"text", "Desktop release is primary focus"},
                                       {"status", "proposed"},
                                       {"revision", 1},
                                       {"updated_at", "2026-08-16T10:00:00Z"}}})},
          {"decisions", Json::array({{{"id", "it_2"},
                                      {"text", "Postpone mobile app until Q4"},
                                      {"status", "confirmed"},
                                      {"revision", 2},
                                      {"pinned", true},
                                      {"updated_at", "2026-08-16T10:05:00Z"}}})},
          {"action_items", Json::array()},
          {"risks", Json::array()},
          {"timeline", Json::array()},
          {"live_notes", Json::array()},
          {"user_notes", Json::array()}}},
        {"questions", Json::array()},
    };
    const auto rendered = render_state_compact(state);
    CHECK(has(rendered, "Top Insights (Dashboard Spotlight - 2 active):"));
    CHECK(has(rendered, "[it_2 confirmed] [decisions] Postpone mobile app until Q4"));
    CHECK(has(rendered, "[it_1 proposed] [key_points] Desktop release is primary focus"));
}

// from
// tests/test_meeting_spotlight_context.py::test_spotlight_excludes_navigation_and_unreviewed_repair_samples
TEST_CASE("test_spotlight_excludes_navigation_and_unreviewed_repair_samples",
          "[test_meeting_spotlight_context]") {
    Json cards = {
        {"key_points", Json::array({{{"id", "raw"},
                                     {"text", "Let's take a look at this."},
                                     {"status", "proposed"},
                                     {"author_type", "system"},
                                     {"author_id", "state_repair"}},
                                    {{"id", "real"},
                                     {"text", "The meal exceeds its $3 budget by 13 cents."},
                                     {"status", "proposed"},
                                     {"author_type", "agent"}}})},
        {"timeline",
         Json::array({{{"id", "beat"}, {"text", "Seasoning."}, {"status", "proposed"}}})},
    };
    CHECK(ids_of(select_spotlight_items(cards)) == std::vector<std::string>{"real"});
    cards["timeline"][0]["pinned"] = true;
    CHECK(select_spotlight_items(cards)[0]["id"] == "beat");
    cards["key_points"][0]["status"] = "confirmed";
    CHECK(contains(ids_of(select_spotlight_items(cards)), "raw"));
}

// from
// tests/test_meeting_spotlight_context.py::test_agent_rewrite_of_legacy_sample_returns_to_spotlight_and_undo_restores_it
TEST_CASE("test_agent_rewrite_of_legacy_sample_returns_to_spotlight_and_undo_restores_it",
          "[test_meeting_spotlight_context]") {
    using namespace fv::meeting_state;
    const fv::FakeClock clock;
    fv::meeting_state::testing::FixedIds ids;
    auto repo = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(clock);
    repo->create_meeting({{"id", "m_rewrite"},
                          {"title", "Test meeting"},
                          {"status", "active"},
                          {"started_at", "2026-01-01T09:00:00.000001"},
                          {"host_token", "host-token"},
                          {"guest_token", "guest-token"},
                          {"cloud_enabled", false},
                          {"spool_dir", "/tmp/spool"}});
    MeetingState state;
    state.meeting_id = "m_rewrite";
    MeetingStateStore store(std::move(state), repo, {}, clock, ids);
    const auto cards = [&store] {
        return store.snapshot()["cards"];
    };

    const auto add = store.apply("system", std::string("state_repair"),
                                 Json::array({{{"op", "add_item"},
                                               {"card", "key_points"},
                                               {"text", "Let's take a look at this."},
                                               {"evidence", Json::array({"sg_known"})}}}))[0];
    REQUIRE(add.ok);
    REQUIRE(add.target_id);
    CHECK(select_spotlight_items(cards()).empty());
    const auto metadata = store.apply("agent", std::string("agent"),
                                      Json::array({{{"op", "update_item"},
                                                    {"id", *add.target_id},
                                                    {"base_revision", 1},
                                                    {"set", {{"data", {{"start_s", 45.0}}}}},
                                                    {"evidence", Json::array({"sg_known"})}}}))[0];
    CHECK(metadata.ok);
    CHECK(select_spotlight_items(cards()).empty());
    const auto rewrite = store.apply(
        "agent", std::string("agent"),
        Json::array({{{"op", "update_item"},
                      {"id", *add.target_id},
                      {"base_revision", 2},
                      {"set", {{"text", "The meal exceeds its $3 budget by 13 cents."}}},
                      {"evidence", Json::array({"sg_known"})}}}))[0];
    REQUIRE(rewrite.ok);
    const auto featured = select_spotlight_items(cards());
    REQUIRE_FALSE(featured.empty());
    CHECK(featured[0]["id"] == *add.target_id);
    REQUIRE(rewrite.seq);
    const auto undone = store.undo(*rewrite.seq, std::string("host"));
    REQUIRE_FALSE(undone.empty());
    CHECK(undone[0].ok);
    CHECK(select_spotlight_items(cards()).empty());
}

TEST_CASE("spotlight text normalization and overlap", "[spotlight]") {
    CHECK(normalized_text("  Alpha, BETA-gamma!! ") == "alpha beta gamma");
    CHECK(normalized_text("Caf\xC3\xA9 au lait") == "caf au lait");
    CHECK(normalized_text("!!!").empty());
    CHECK(texts_overlap("a b c", "a b c"));
    CHECK(texts_overlap("alpha beta gamma delta", "alpha beta gamma delta epsilon"));
    // Containment: three of the smaller set's four words, Jaccard only 3/8.
    CHECK(texts_overlap("alpha beta gamma delta", "alpha beta gamma one two three four"));
    CHECK_FALSE(texts_overlap("alpha beta gamma", "alpha beta one two three four five"));
    CHECK_FALSE(texts_overlap("one two three", "one four five"));
    CHECK_FALSE(texts_overlap("", "one"));
}
