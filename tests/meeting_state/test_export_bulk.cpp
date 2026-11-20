// tests/test_meeting_export_bulk.py: bulk Past Meetings export assembly.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "meeting_state/export_bulk.h"
#include "pyport/json.h"
#include "stub_repository.h"
#include "testkit/testkit.h"

using fv::Json;
using namespace fv::meeting_state;
using fv::meeting_state::test_support::StubRepository;

namespace {

Json row(const std::string& id, const std::string& status = "ended",
         const std::string& started = "2026-03-15T14:30:00", const std::string& title = "Planning",
         bool has_transcript = true) {
    return {{"id", id},
            {"title", title},
            {"status", status},
            {"started_at", started},
            {"content_summary", {{"has_transcript", has_transcript}}}};
}

MeetingExport entry(const std::string& id = "m_1", const std::string& title = "Planning") {
    MeetingExport out;
    out.meeting = {{"id", id},
                   {"title", title},
                   {"started_at", "2026-03-15T14:30:00"},
                   {"ended_at", "2026-03-15T15:00:00"}};
    out.state = {{"title", title}, {"participants", Json::object()}, {"cards", Json::object()}};
    out.segments = {Json::parse(
        R"({"id": "sg_1", "start_s": 1.0, "end_s": 2.0, "text": "Hello", "channel": "mic"})")};
    return out;
}

std::vector<std::string> ids(const std::vector<Json>& rows) {
    std::vector<std::string> out;
    for (const auto& r : rows) {
        out.push_back(r["id"].get<std::string>());
    }
    return out;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

const fv::FakeClock& ny_clock() {
    static const fv::FakeClock pinned(fv::FakeClock::default_utc(), "America/New_York");
    return pinned;
}

} // namespace

// from tests/test_meeting_export_bulk.py::test_list_export_meetings_skips_live_and_fills_summary
TEST_CASE("test_list_export_meetings_skips_live_and_fills_summary", "[test_meeting_export_bulk]") {
    class Repo final : public StubRepository {
    public:
        std::vector<std::string> summarized;
        std::vector<Json> list_meetings() override {
            return {Json{{"id", "m_live"}, {"status", "active"}},
                    Json{{"id", "m_done"}, {"status", "ended"}, {"title", "Done"}}};
        }
        std::vector<Json> get_audio_chunks(std::string_view meeting_id) override {
            summarized.emplace_back(meeting_id);
            return {};
        }
        std::vector<Json> get_segments(std::string_view, double,
                                       std::optional<std::int64_t>) override {
            return {};
        }
    } repo;
    const auto meetings = list_export_meetings(repo);
    CHECK(ids(meetings) == std::vector<std::string>{"m_done"});
    CHECK(meetings[0]["content_summary"]["has_transcript"] == false);
    CHECK(repo.summarized == std::vector<std::string>{"m_done"});
}

// from tests/test_meeting_export_bulk.py::test_filter_export_meetings_applies_date_and_transcript
TEST_CASE("test_filter_export_meetings_applies_date_and_transcript", "[test_meeting_export_bulk]") {
    const std::vector<Json> rows = {
        row("m_old", "ended", "2026-01-01T10:00:00"),
        row("m_mid", "ended", "2026-03-15T14:30:00"),
        row("m_new", "ended", "2026-06-01T09:00:00"),
        row("m_silent", "ended", "2026-03-20T09:00:00", "Planning", false),
        row("m_live", "active", "2026-03-15T14:30:00"),
        row("m_undated", "ended", ""),
    };
    const auto filtered =
        filter_export_meetings(rows, fv::py::DateTime{2026, 3, 1},
                               fv::py::DateTime{2026, 3, 31, 23, 59, 59}, true, ny_clock());
    CHECK(ids(filtered) == std::vector<std::string>{"m_mid"});
}

// from
// tests/test_meeting_export_bulk.py::test_filter_keeps_unknown_transcript_when_only_with_transcript
TEST_CASE("test_filter_keeps_unknown_transcript_when_only_with_transcript",
          "[test_meeting_export_bulk]") {
    Json unknown = row("m_unknown");
    unknown.erase("content_summary");
    CHECK(ids(filter_export_meetings({unknown}, std::nullopt, std::nullopt, true, ny_clock())) ==
          std::vector<std::string>{"m_unknown"});
}

// from tests/test_meeting_export_bulk.py::test_collect_meeting_export_parses_state_json
TEST_CASE("test_collect_meeting_export_parses_state_json", "[test_meeting_export_bulk]") {
    class Repo final : public StubRepository {
    public:
        std::optional<Json> get_meeting(std::string_view meeting_id) override {
            return Json{{"id", meeting_id}, {"state_json", R"({"title": "From JSON"})"}};
        }
        std::vector<Json> get_segments(std::string_view, double,
                                       std::optional<std::int64_t>) override {
            return {Json{{"text", "Hi"}}};
        }
    } repo;
    const auto collected = collect_meeting_export(repo, "m_1");
    REQUIRE(collected);
    CHECK(collected->state["title"] == "From JSON");
    CHECK(collected->segments == std::vector<Json>{Json{{"text", "Hi"}}});
}

// from tests/test_meeting_export_bulk.py::test_collect_meeting_export_missing_returns_none
TEST_CASE("test_collect_meeting_export_missing_returns_none", "[test_meeting_export_bulk]") {
    class Repo final : public StubRepository {
    public:
        std::optional<Json> get_meeting(std::string_view) override { return std::nullopt; }
    } repo;
    CHECK_FALSE(collect_meeting_export(repo, "m_gone").has_value());
}

// from tests/test_meeting_export_bulk.py::test_render_markdown_honors_content_toggles
TEST_CASE("test_render_markdown_honors_content_toggles", "[test_meeting_export_bulk]") {
    MeetingExport e = entry();
    e.state["cards"] = Json::parse(R"({"decisions": [{"id": "it_1", "card": "decisions",
        "text": "Ship it", "status": "confirmed", "data": {}, "pinned": false, "evidence": []}]})");
    const std::string full = render_meeting_document(e, FORMAT_MARKDOWN, {}, ny_clock());
    const std::string slim =
        render_meeting_document(e, FORMAT_MARKDOWN, {false, false}, ny_clock());
    CHECK(has(full, "Hello"));
    CHECK(has(full, "## Decisions"));
    CHECK_FALSE(has(slim, "Hello"));
    CHECK_FALSE(has(slim, "## Decisions"));
    CHECK(has(slim, "Planning"));
}

// from tests/test_meeting_export_bulk.py::test_json_and_txt_ignore_content_toggles
TEST_CASE("test_json_and_txt_ignore_content_toggles", "[test_meeting_export_bulk]") {
    const MeetingExport e = entry();
    const Json json_doc =
        fv::py::loads(render_meeting_document(e, FORMAT_JSON, {false, false}, ny_clock()));
    CHECK(json_doc["segments"][0]["text"] == "Hello");
    CHECK(has(render_meeting_document(e, FORMAT_TXT, {false, true}, ny_clock()), "Hello"));
}

// from tests/test_meeting_export_bulk.py::test_combined_markdown_has_header
TEST_CASE("test_combined_markdown_has_header", "[test_meeting_export_bulk]") {
    // datetime.now() is pinned to 2026-04-01 12:00 local.
    const fv::FakeClock pinned(std::chrono::sys_days{std::chrono::year{2026} / 4 / 1} +
                                   std::chrono::hours{16},
                               "America/New_York");
    const std::string document =
        render_combined({entry(), entry("m_2", "Retro")}, FORMAT_MARKDOWN, {}, pinned);
    CHECK(document.starts_with("# Past Meetings Export\n"));
    CHECK(has(document, "2 meeting(s) \xC2\xB7 exported 2026-04-01 12:00"));
    CHECK(has(document, "Planning"));
    CHECK(has(document, "Retro"));
}

// from tests/test_meeting_export_bulk.py::test_write_per_meeting_files_disambiguates
TEST_CASE("test_write_per_meeting_files_disambiguates", "[test_meeting_export_bulk]") {
    fv::testkit::TempDataRoot tmp;
    const auto written = write_per_meeting_files(
        tmp.path(), FORMAT_TXT, {entry("m_1", "Planning"), entry("m_2", "Planning")}, {},
        ny_clock());
    REQUIRE(written.size() == 2);
    CHECK(written[0].filename() != written[1].filename());
    for (const auto& path : written) {
        CHECK(fv::path_to_utf8(path.filename()).ends_with(".txt"));
    }
    CHECK_FALSE(read_bytes(written[0]).empty());
}

// from
// tests/test_meeting_export_bulk.py::test_write_per_meeting_files_never_overwrites_prior_export
TEST_CASE("test_write_per_meeting_files_never_overwrites_prior_export",
          "[test_meeting_export_bulk]") {
    fv::testkit::TempDataRoot tmp;
    const MeetingExport e = entry("m_a", "Existing meeting");
    const std::string stem = meeting_file_stem(e.meeting, e.state, ny_clock());
    const auto original = tmp.path() / fv::path_from_utf8(stem + ".txt");
    {
        std::ofstream out(original, std::ios::binary);
        out << "keep me";
    }
    const auto written = write_per_meeting_files(tmp.path(), FORMAT_TXT, {e}, {}, ny_clock());
    CHECK(read_bytes(original) == "keep me");
    CHECK(written ==
          std::vector<std::filesystem::path>{tmp.path() / fv::path_from_utf8(stem + "-2.txt")});
}

// from tests/test_meeting_export_bulk.py::test_meeting_file_stem_is_filesystem_safe
TEST_CASE("test_meeting_file_stem_is_filesystem_safe", "[test_meeting_export_bulk]") {
    const std::string stem =
        meeting_file_stem({{"title", "Q3 / Roadmap?"}, {"started_at", "2026-03-15T14:30:00"}},
                          {{"title", "Q3 / Roadmap?"}}, ny_clock());
    CHECK_FALSE(has(stem, "/"));
    CHECK_FALSE(has(stem, "?"));
    CHECK(has(stem, "20260315_143000"));
}

TEST_CASE("bulk: combined text and JSON, per-meeting Markdown names and text-mode files",
          "[meeting_state_export]") {
    const std::string text =
        render_combined({entry(), entry("m_2", "Retro")}, FORMAT_TXT, {}, ny_clock());
    CHECK(has(text, "\n\n" + std::string(60, '=') + "\n\n"));
    const Json combined = fv::py::loads(render_combined({entry()}, FORMAT_JSON, {}, ny_clock()));
    CHECK(combined["format_version"] == 1);
    CHECK(combined["exported_at"] == "2025-12-31T19:00:00");
    CHECK(combined["meetings"][0]["meeting"]["id"] == "m_1");
    fv::testkit::TempDataRoot tmp;
    const auto written =
        write_per_meeting_files(tmp.path() / "nested", FORMAT_MARKDOWN, {entry()}, {}, ny_clock());
    REQUIRE(written.size() == 1);
    CHECK(fv::path_to_utf8(written[0].filename()) == "20260315_143000_Planning.markdown");
#ifdef _WIN32
    CHECK(has(read_bytes(written[0]), "\r\n"));
#endif
    CHECK(meeting_file_stem({{"title", "  ...  "}}, Json::object(), ny_clock()) == "unknown_date");
}
