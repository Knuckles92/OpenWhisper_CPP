// Consent-gated past-meeting recall: caps, exclusion, opaque refs (OpenWhisper's
// tests/test_meeting_recall.py over a stub source like its _Repo), then the same calls through
// the in-memory and the SQL meeting repositories.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "intel_support.h"
#include "meeting_intel/history_source.h"
#include "meeting_intel/recall.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/text.h"
#include "testkit/testkit.h"

using namespace fv::meeting_intel;
using namespace fv::meeting_intel::test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

Json hit(const std::string& meeting_id = "m_past",
         const std::string& text = "we adopted the budget", const std::string& title = "Q1 review",
         const std::string& segment_id = "sg_abcdef123456") {
    return Json{{"segment_id", segment_id},
                {"meeting_id", meeting_id},
                {"text", text},
                {"snippet", text},
                {"title", title},
                {"started_at", "2026-03-01T10:00:00"},
                {"start_s", 12.0}};
}

RecallDeps deps_for(HistorySource* history, bool enabled) {
    RecallDeps deps;
    deps.history = history;
    deps.settings = fixed(Json{{"meeting_past_recall_enabled", enabled}});
    return deps;
}

std::string text_of(const Json& result) {
    return result.at("text").get<std::string>();
}

} // namespace

// from tests/test_meeting_recall.py::TestRecallConsent::test_disabled_when_setting_is_off
TEST_CASE("TestRecallConsent::test_disabled_when_setting_is_off", "[test_meeting_recall]") {
    StubHistory repo;
    repo.rows = {hit()};
    RecallDeps deps = deps_for(&repo, false);
    const Json result = search_past_meetings(deps, "budget", "m_live", "");
    CHECK(result.at("disabled") == true);
    CHECK(result.at("ok") == false);
    CHECK(result.at("hits") == Json::array());
    CHECK_THAT(fv::py::lower(text_of(result)), ContainsSubstring("disabled"));
    CHECK_FALSE(repo.search_call);
}

// from tests/test_meeting_recall.py::TestRecallConsent::test_missing_repository_is_disabled
TEST_CASE("TestRecallConsent::test_missing_repository_is_disabled", "[test_meeting_recall]") {
    RecallDeps deps = deps_for(nullptr, true);
    const Json result = search_past_meetings(deps, "budget", "m_live", "");
    CHECK(result.at("disabled") == true);
}

// from
// tests/test_meeting_recall.py::TestRecallSearch::test_excludes_current_meeting_and_omits_sg_ids
TEST_CASE("TestRecallSearch::test_excludes_current_meeting_and_omits_sg_ids",
          "[test_meeting_recall]") {
    StubHistory repo;
    repo.rows = {hit("m_live", "live budget talk"),
                 hit("m_past", "we adopted the budget", "Q1 review", "sg_deadbeef9999")};
    RecallDeps deps = deps_for(&repo, true);
    const Json result = search_past_meetings(deps, "budget", "m_live", "");
    CHECK(result.at("ok") == true);
    REQUIRE(repo.search_call);
    CHECK(repo.search_call->exclude_meeting_id == std::optional<std::string>("m_live"));
    REQUIRE(result.at("hits").size() == 1);
    const Json& first = result.at("hits").at(0);
    CHECK(first.at("meeting_id") == "m_past");
    CHECK(first.at("ref").get<std::string>().starts_with("past:m_past:"));
    CHECK(text_of(result).find("sg_") == std::string::npos);
    CHECK(first.at("ref").get<std::string>().find("sg_") == std::string::npos);
    CHECK(first.at("snippet").get<std::string>().find("sg_") == std::string::npos);
}

// from tests/test_meeting_recall.py::TestRecallSearch::test_empty_query_asks_for_input
TEST_CASE("TestRecallSearch::test_empty_query_asks_for_input", "[test_meeting_recall]") {
    StubHistory repo;
    repo.rows = {hit()};
    RecallDeps deps = deps_for(&repo, true);
    const Json result = search_past_meetings(deps, "   ", "m_live", "");
    CHECK(result.at("ok") == false);
    CHECK(result.at("hits") == Json::array());
    CHECK_FALSE(repo.search_call);
}

// from tests/test_meeting_recall.py::TestRecallSearch::test_clamps_limit_and_snippet
TEST_CASE("TestRecallSearch::test_clamps_limit_and_snippet", "[test_meeting_recall]") {
    std::string long_text = "budget ";
    for (int index = 0; index < 80; ++index) {
        long_text += "word ";
    }
    StubHistory repo;
    for (int index = 0; index < 30; ++index) {
        repo.rows.push_back(
            hit("m_" + std::to_string(index), long_text, "Meet " + std::to_string(index)));
    }
    RecallDeps deps = deps_for(&repo, true);
    const Json result = search_past_meetings(deps, "budget", "m_live", "", Json(99));
    REQUIRE(repo.search_call);
    CHECK(repo.search_call->limit == kRecallMaxHitLimit);
    CHECK(result.at("hits").size() <= static_cast<std::size_t>(kRecallMaxHitLimit));
    for (const auto& item : result.at("hits")) {
        CHECK(fv::py::len(item.at("snippet").get<std::string>()) <= kRecallMaxSnippetChars);
    }
}

// from
// tests/test_meeting_recall.py::TestRecallSearch::test_sanitizes_sg_ids_embedded_in_transcript_text
TEST_CASE("TestRecallSearch::test_sanitizes_sg_ids_embedded_in_transcript_text",
          "[test_meeting_recall]") {
    StubHistory repo;
    repo.rows = {hit("m_past", "see sg_abcdef123456 from last week")};
    RecallDeps deps = deps_for(&repo, true);
    const Json result = search_past_meetings(deps, "last", "m_live", "");
    CHECK(text_of(result).find("sg_") == std::string::npos);
    CHECK_THAT(text_of(result), ContainsSubstring("[id]"));
}

// from tests/test_meeting_recall.py::TestRecallSlice::test_rejects_current_meeting_id
TEST_CASE("TestRecallSlice::test_rejects_current_meeting_id", "[test_meeting_recall]") {
    StubHistory repo;
    repo.meetings["m_live"] = Json{{"id", "m_live"}, {"title", "Now"}};
    RecallDeps deps = deps_for(&repo, true);
    const Json result = search_past_meetings(deps, "", "m_live", "m_live");
    CHECK(result.at("ok") == false);
    CHECK_THAT(text_of(result), ContainsSubstring("current meeting"));
}

// from tests/test_meeting_recall.py::TestRecallSlice::test_returns_opaque_slice
TEST_CASE("TestRecallSlice::test_returns_opaque_slice", "[test_meeting_recall]") {
    StubHistory repo;
    repo.meetings["m_past"] =
        Json{{"id", "m_past"}, {"title", "Prior sync"}, {"started_at", "2026-02-02T09:00:00"}};
    repo.segments["m_past"] = {
        Json{{"id", "sg_aaa111222333"}, {"text", "Ship Friday"}, {"start_s", 3.5}},
        Json{{"id", "sg_bbb111222333"}, {"text", "Agreed"}, {"start_s", 8.0}}};
    RecallDeps deps = deps_for(&repo, true);
    const Json result = search_past_meetings(deps, "", "m_live", "m_past");
    CHECK(result.at("ok") == true);
    CHECK_THAT(text_of(result), ContainsSubstring("Prior sync"));
    CHECK(text_of(result).find("sg_") == std::string::npos);
    CHECK(result.at("hits").at(0).at("ref") == "past:m_past:1");
    CHECK(result.at("hits").at(1).at("ref") == "past:m_past:2");
}

TEST_CASE("a failed settings read leaves recall off", "[test_meeting_recall]") {
    StubHistory repo;
    RecallDeps deps;
    deps.history = &repo;
    deps.settings = []() -> Json {
        throw std::runtime_error("unreadable");
    };
    const Json result = search_past_meetings(deps, "budget", "m_live", "");
    CHECK(result.at("disabled") == true);
    CHECK(text_of(result) == "Past-meeting recall is disabled.");
}

TEST_CASE("a failing source reads as a failed recall", "[test_meeting_recall]") {
    struct Failing final : HistorySource {
        std::vector<Json> search_transcripts(std::string_view, const std::optional<std::string>&,
                                             std::int64_t) override {
            throw std::runtime_error("database is locked");
        }
        std::vector<Json> search_candidates(std::string_view, const std::optional<std::string>&,
                                            std::int64_t, bool) override {
            return {};
        }
        std::optional<Json> get_meeting(std::string_view) override {
            throw std::runtime_error("database is locked");
        }
        std::vector<Json> get_segments(std::string_view, std::int64_t) override { return {}; }
    } failing;
    RecallDeps deps = deps_for(&failing, true);
    LogCapture log;
    CHECK(search_past_meetings(deps, "budget", "m_live", "") ==
          Json{{"ok", false}, {"text", "Past-meeting recall failed."}, {"hits", Json::array()}});
    CHECK(text_of(search_past_meetings(deps, "", "m_live", "m_past")) ==
          "Past-meeting recall failed.");
    // Logged without the error's text, which could quote stored transcript text.
    CHECK(log.count("Past-meeting recall failed") == 2);
    CHECK(log.text().find("locked") == std::string::npos);
}

TEST_CASE("an infinite limit raises as Python's int() does", "[test_meeting_recall]") {
    StubHistory repo;
    RecallDeps deps = deps_for(&repo, true);
    CHECK_THROWS_AS(search_past_meetings(deps, "budget", "m_live", "",
                                         Json(std::numeric_limits<double>::infinity())),
                    fv::py::OverflowError);
}

TEST_CASE("recall reads the in-memory meeting repository", "[test_meeting_recall]") {
    fv::FakeClock clock;
    fv::meeting_state::testing::FakeMeetingRepository repository(clock);
    for (const auto& [id, cloud] : {std::pair{"m_past", true}, std::pair{"m_live", true}}) {
        repository.create_meeting({{"id", id},
                                   {"title", std::string("Meeting ") + id},
                                   {"status", "ended"},
                                   {"started_at", "2026-02-02T09:00:00"},
                                   {"host_token", std::string(id) + "h"},
                                   {"guest_token", std::string(id) + "g"},
                                   {"cloud_enabled", cloud},
                                   {"spool_dir", "/tmp/spool"}});
    }
    fv::meeting_state::TranscriptSegment past;
    past.segment_id = "sg_aaa111222333";
    past.meeting_id = "m_past";
    past.channel = "mic";
    past.start_s = 4.0;
    past.end_s = 6.0;
    past.text = "We adopted the budget for March";
    fv::meeting_state::TranscriptSegment live = past;
    live.segment_id = "sg_bbb111222333";
    live.meeting_id = "m_live";
    live.text = "The budget again, live";
    repository.add_segments({past, live});
    RepositoryHistorySource history(repository);
    RecallDeps deps = deps_for(&history, true);

    const Json found = search_past_meetings(deps, "budget", "m_live", "");
    REQUIRE(found.at("hits").size() == 1);
    CHECK(found.at("hits").at(0).at("ref") == "past:m_past:1");
    CHECK(text_of(found) ==
          "past:m_past:1  \"Meeting m_past\" (2026-02-02) t=4s — We adopted the budget for March");

    const Json slice = search_past_meetings(deps, "", "m_live", "m_past");
    CHECK(text_of(slice) ==
          "Meeting \"Meeting m_past\" (2026-02-02)\npast:m_past:1  t=4s  We adopted the budget "
          "for March");
    CHECK(text_of(search_past_meetings(deps, "", "m_live", "m_gone")) == "Unknown meeting.");
}

TEST_CASE("recall reads the SQL meeting repository", "[test_meeting_recall]") {
    fv::testkit::TempDataRoot root;
    fv::FakeClock clock;
    auto database =
        std::make_unique<fv::Database>(fv::Database::open_or_create(root.path() / "fiorivoce.db"));
    fv::meeting_state::SqlMeetingRepository repository(*database, clock);
    repository.create_meeting({{"id", "m_past"},
                               {"title", "Budget review"},
                               {"status", "ended"},
                               {"started_at", "2026-02-02T09:00:00"},
                               {"host_token", "h"},
                               {"guest_token", "g"},
                               {"cloud_enabled", false},
                               {"spool_dir", "/tmp/spool"}});
    fv::meeting_state::TranscriptSegment segment;
    segment.segment_id = "sg_aaa111222333";
    segment.meeting_id = "m_past";
    segment.channel = "mic";
    segment.start_s = 12.5;
    segment.end_s = 14.0;
    segment.text = "We adopted the budget";
    repository.add_segments({segment});
    RepositoryHistorySource history(repository);
    RecallDeps deps = deps_for(&history, true);

    const Json found = search_past_meetings(deps, "budget", "m_live", "");
    REQUIRE(found.at("hits").size() == 1);
    // The full-text index marks the match in its snippet.
    CHECK(text_of(found) ==
          "past:m_past:1  \"Budget review\" (2026-02-02) t=12.5s — We adopted the [budget]");
    CHECK(history.search_candidates("budget review", std::nullopt, 48, false).size() == 1);
    CHECK(history.search_candidates("budget review", std::nullopt, 48, true).empty());
}
