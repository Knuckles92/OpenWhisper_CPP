// SqlMeetingRepository::search_candidates: the passages a semantic ranking may judge (the two
// candidate tests of OpenWhisper's tests/test_meeting_fast_features.py, then the budget split).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>
#include <vector>

#include "sql_support.h"

using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;
using fv::Json;

namespace {

void add_meeting(SqlMeetingRepository& repo, const std::string& id, bool cloud,
                 const std::string& started_at = "2026-09-18T10:00:00") {
    repo.create_meeting({{"id", id},
                         {"title", "Launch"},
                         {"status", "ended"},
                         {"started_at", started_at},
                         {"host_token", id},
                         {"guest_token", id + "g"},
                         {"cloud_enabled", cloud},
                         {"spool_dir", "/tmp/spool"}});
}

std::vector<std::string> meeting_ids(const std::vector<Json>& rows) {
    return ids_of(rows, "meeting_id");
}

} // namespace

// from
// tests/test_meeting_fast_features.py::test_semantic_candidates_include_non_keyword_passages_and_exclude_offline
TEST_CASE("test_semantic_candidates_include_non_keyword_passages_and_exclude_offline",
          "[test_meeting_fast_features]") {
    SqlFixture f;
    for (const auto& [id, cloud, text] :
         {std::tuple{"m_online", true, "We agreed to postpone the launch until November."},
          std::tuple{"m_private", false, "The release was delayed for a private reason."}}) {
        add_meeting(*f.repo, id, cloud);
        f.repo->add_segments({make_segment(id, std::string("sg_") + id, 10, 15, text, "mic")});
    }
    const std::vector<Json> rows = f.repo->search_candidates("shipping delay");
    CHECK(meeting_ids(rows) == std::vector<std::string>{"m_online"});
    CHECK(f.repo->search_candidates("shipping delay", std::string("m_online")).empty());
}

// from
// tests/test_meeting_fast_features.py::test_semantic_candidates_fall_back_to_recent_passages_without_the_search_index
TEST_CASE("test_semantic_candidates_fall_back_to_recent_passages_without_the_search_index",
          "[test_meeting_fast_features]") {
    SqlFixture f;
    add_meeting(*f.repo, "m_online", true);
    f.repo->add_segments({make_segment("m_online", "sg_m_online", 10, 15,
                                       "We agreed to postpone the launch.", "mic")});
    const std::vector<Json> triggers = column_values(
        *f.database, "SELECT name FROM sqlite_master WHERE type = 'trigger' AND sql LIKE "
                     "'%meeting_segments_fts%'");
    f.database->write([&](fv::sql::Connection& connection) {
        for (const auto& trigger : triggers) {
            connection.exec("DROP TRIGGER \"" + trigger.get<std::string>() + "\"");
        }
        connection.exec("DROP TABLE meeting_segments_fts");
    });
    const std::vector<Json> rows = f.repo->search_candidates("postpone launch");
    CHECK(meeting_ids(rows) == std::vector<std::string>{"m_online"});
}

TEST_CASE("candidates keep half the budget for term matches and fill it with recent passages",
          "[search_candidates]") {
    SqlFixture f;
    add_meeting(*f.repo, "m_old", false, "2026-01-01T10:00:00");
    add_meeting(*f.repo, "m_new", false, "2026-06-01T10:00:00");
    std::vector<TranscriptSegment> segments;
    // Four minutes of m_old mention the budget, each passage longer than the one before (bm25
    // ranks the shorter first); m_new never does.
    for (int minute = 0; minute < 4; ++minute) {
        std::string text = "The budget for minute " + std::to_string(minute);
        for (int word = 0; word < minute; ++word) {
            text += " again";
        }
        segments.push_back(make_segment("m_old", "sg_old" + std::to_string(minute), minute * 60.0,
                                        minute * 60.0 + 5, text));
    }
    for (int minute = 0; minute < 4; ++minute) {
        // Two passages per minute: only the longer one is a candidate.
        segments.push_back(make_segment("m_new", "sg_new" + std::to_string(minute) + "a",
                                        minute * 60.0, minute * 60.0 + 5,
                                        "Short words only here, nothing more"));
        segments.push_back(make_segment(
            "m_new", "sg_new" + std::to_string(minute) + "b", minute * 60.0 + 10,
            minute * 60.0 + 15, "A much longer passage about the venue and catering costs"));
    }
    f.repo->add_segments(segments);

    const std::vector<Json> rows =
        f.repo->search_candidates("What was the budget?", std::nullopt, 4, false);
    const std::vector<std::string> ids = ids_of(rows, "segment_id");
    REQUIRE(ids.size() == 4);
    // The two best term matches, then the recent list: the newest passage of each meeting,
    // newest meeting first.
    CHECK(ids == std::vector<std::string>{"sg_old0", "sg_old1", "sg_new3b", "sg_old3"});
    for (const auto& row : rows) {
        CHECK(row.size() == 7);
        CHECK(row.contains("text"));
        CHECK(row.contains("title"));
        CHECK(row.contains("started_at"));
    }
    // Only stop words or short words: no term search, recent passages only.
    const std::vector<Json> recent =
        f.repo->search_candidates("what was the meeting", std::nullopt, 48, false);
    CHECK(recent.size() == 8);
    // The limit is clamped to 1..48.
    CHECK(f.repo->search_candidates("budget", std::nullopt, 0, false).size() == 1);
    CHECK(f.repo->search_candidates("budget", std::nullopt, 500, false).size() == 8);
}
