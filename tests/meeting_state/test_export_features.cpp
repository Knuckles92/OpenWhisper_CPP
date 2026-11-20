// tests/test_meeting_export_features.py: export compatibility for features added after the
// original export dialogs. test_web_downloads_preserve_current_features_and_match_bulk_content
// is M7.16's (the /api/export route).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "export_fixture.h"
#include "history/entry.h"
#include "history/export.h"
#include "meeting_state/export_bulk.h"
#include "meeting_state/export_json.h"
#include "meeting_state/export_markdown.h"
#include "meeting_state/export_txt.h"
#include "pyport/json.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using fv::meeting_state::test_support::ExportFixture;

namespace {

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

// _recent_features(): the export fixture with review, citation, pulse and note features.
ExportFixture recent_features() {
    ExportFixture f;
    Json& action = f.state["cards"]["action_items"][0];
    action["revision"] = 2;
    action["review"] = {{"state", "human"}};
    action["citation_check"] = {{"status", "contradicted"}, {"revision", 2}};
    action["data"]["deadline"] = "Friday";
    f.state["insight_review"] = Json::parse(R"({"questions": [
        {"correction": "The RFC is due Friday.", "status": "answered"},
        {"correction": "OUTDATED CLARIFICATION", "superseded": true}]})");
    f.state["live_highlights"] = Json::parse(R"([
        {"id": "late", "kind": "number", "start_s": 90, "text": "Budget is 5000",
         "segment_id": "sg_2", "probability": 0.9,
         "assessment": {"threshold": 0.8, "scores": {"number": 0.9}}},
        {"id": "lesson", "kind": "takeaway", "start_s": 45,
         "text": "Early customer feedback prevents rework", "segment_id": "sg_1",
         "probability": 0.93},
        {"id": "early", "kind": "decision", "start_s": 5, "text": "Choose June",
         "segment_id": "sg_1", "probability": 0.95}])");
    f.state["cards"]["live_notes"] = Json::parse(R"([
        {"id": "late-note", "card": "live_notes", "status": "proposed",
         "author_type": "system", "author_id": "voice_command",
         "text": "Keep the budget", "data": {"heading": "Budget", "start_s": 90}},
        {"id": "early-note", "card": "live_notes", "status": "proposed",
         "text": "Opening note", "data": {"start_s": 5}},
        {"id": "removed-note", "status": "removed", "text": "REMOVED NOTE"}])");
    return f;
}

} // namespace

// from
// tests/test_meeting_export_features.py::test_markdown_keeps_recent_features_without_mutating_snapshot
TEST_CASE("test_markdown_keeps_recent_features_without_mutating_snapshot",
          "[test_meeting_export_features]") {
    const ExportFixture f = recent_features();
    const Json original = f.state;
    const std::string md = export_markdown(f.meeting, f.state, f.segments, true, true, f.clock);
    for (const char* text :
         {"## Meeting Pulses", "[00:05] **Decision:** Choose June",
          "[01:30] **Number:** Budget is 5000",
          "[00:45] **Takeaways:** Early customer feedback prevents rework",
          "Spoken note: Keep the budget", "User clarified: Draft RFC", "Due: Friday",
          "Advisory: Citation conflicts with claim", "The RFC is due Friday."}) {
        INFO(text);
        CHECK(has(md, text));
    }
    CHECK(md.find("Choose June") < md.find("Early customer feedback"));
    CHECK(md.find("Early customer feedback") < md.find("Budget is 5000"));
    CHECK(md.find("Opening note") < md.find("Keep the budget"));
    CHECK_FALSE(has(md, "OUTDATED CLARIFICATION"));
    CHECK_FALSE(has(md, "REMOVED NOTE"));
    CHECK(f.state == original);
}

// from
// tests/test_meeting_export_features.py::test_markdown_citation_badges_only_describe_current_revision
TEST_CASE("test_markdown_citation_badges_only_describe_current_revision",
          "[test_meeting_export_features]") {
    const auto [status, label] = GENERATE(table<std::string, std::string>({
        {"supported", "Citation supports claim"},
        {"contradicted", "Citation conflicts with claim"},
        {"unsupported", "Check citation"},
        {"missing", "Missing citation"},
        {"uncertain", "Citation uncertain"},
        {"unavailable", "Citation check unavailable"},
        {"stale", "Citation needs recheck"},
    }));
    ExportFixture f;
    Json& item = f.state["cards"]["key_points"][0];
    item["revision"] = 3;
    item["citation_check"] = {{"status", status}, {"revision", 3}};
    CHECK(has(export_markdown(f.meeting, f.state, f.segments, true, true, f.clock),
              "Advisory: " + label));
    item["revision"] = 4;
    CHECK_FALSE(has(export_markdown(f.meeting, f.state, f.segments, true, true, f.clock),
                    "Advisory: " + label));
}

// from
// tests/test_meeting_export_features.py::test_intelligence_toggle_omits_new_features_and_legacy_due_date_is_retained
TEST_CASE("test_intelligence_toggle_omits_new_features_and_legacy_due_date_is_retained",
          "[test_meeting_export_features]") {
    ExportFixture f = recent_features();
    Json& data = f.state["cards"]["action_items"][0]["data"];
    data["due_date"] = data["deadline"];
    data.erase("deadline");
    CHECK(has(export_markdown(f.meeting, f.state, f.segments, true, true, f.clock), "Due: Friday"));
    const std::string md = export_markdown(f.meeting, f.state, f.segments, true, false, f.clock);
    for (const char* text :
         {"Meeting Pulses", "Budget is 5000", "Early customer feedback prevents rework",
          "Spoken note", "User Clarifications", "Due: Friday", "Advisory:"}) {
        INFO(text);
        CHECK_FALSE(has(md, text));
    }
    CHECK(has(md, "Welcome everyone"));
}

// from tests/test_meeting_export_features.py::test_json_preserves_all_new_state_and_raw_transcript
TEST_CASE("test_json_preserves_all_new_state_and_raw_transcript",
          "[test_meeting_export_features]") {
    ExportFixture f = recent_features();
    f.segments[0]["original_text"] = "Original recognizer text";
    const Json payload = fv::py::loads(export_json(f.meeting, f.state, f.segments));
    CHECK(payload["state"] == f.state);
    CHECK(payload["segments"] == Json(f.segments));
    CHECK_FALSE(payload["meeting"].contains("host_token"));
    CHECK_FALSE(payload["meeting"].contains("guest_token"));
}

// from
// tests/test_meeting_export_features.py::test_bulk_exports_corrected_speech_and_json_preserves_raw_text
TEST_CASE("test_bulk_exports_corrected_speech_and_json_preserves_raw_text",
          "[test_meeting_export_features]") {
    const std::string author = GENERATE("user", "voice_command");
    test_support::SqlFixture db;
    const std::string meeting_id = test_support::make_meeting(*db.repo);
    db.repo->add_segments(
        {test_support::make_segment(meeting_id, "sg_aaa111", 1.0, 3.0, "Myra will send it")});
    Json note = {{"id", "fix"},
                 {"card", "user_notes"},
                 {"text", "Correct the name"},
                 {"status", "edited"},
                 {"author_type", author == "user" ? "user" : "system"},
                 {"author_id", author == "user" ? Json() : Json("voice_command")},
                 {"data",
                  {{"kind", "term_correction"},
                   {"selected_text", "Myra"},
                   {"replacement", "Maya"},
                   {"source", "voice_command"},
                   {"command", "fix_transcript"}}}};
    Json state = {{"seq", 1}, {"cards", {{"user_notes", Json::array({note})}}}};
    db.repo->persist_state(meeting_id, state);
    const auto collected = collect_meeting_export(*db.repo, meeting_id);
    REQUIRE(collected);
    for (const auto format : {FORMAT_MARKDOWN, FORMAT_TXT}) {
        CHECK(has(render_combined({*collected}, format, {}, db.clock), "Maya will send it"));
    }
    const Json payload =
        fv::py::loads(render_combined({*collected}, FORMAT_JSON, {}, db.clock))["meetings"][0];
    CHECK(payload["segments"][0]["text"] == "Maya will send it");
    CHECK(payload["segments"][0]["original_text"] == "Myra will send it");
    CHECK_FALSE(has(payload.dump(), "host-token"));
    state["cards"]["user_notes"][0]["status"] = "removed";
    state["seq"] = 2;
    db.repo->persist_state(meeting_id, state);
    const auto restored = collect_meeting_export(*db.repo, meeting_id);
    REQUIRE(restored);
    CHECK(has(render_combined({*restored}, FORMAT_TXT, {}, db.clock), "Myra will send it"));
}

// from
// tests/test_meeting_export_features.py::test_utc_headers_use_local_display_and_legacy_dates_still_work
TEST_CASE("test_utc_headers_use_local_display_and_legacy_dates_still_work",
          "[test_meeting_export_features]") {
    // The machine's zone is pinned: 2026-09-20T01:30Z is 21:30 the day before in New York.
    const fv::FakeClock clock(fv::FakeClock::default_utc(), "America/New_York");
    const Json meeting = {{"started_at", "2026-09-20T01:30:00+00:00"}};
    CHECK(format_meeting_date(meeting, clock) == "2026-09-19 21:30");
    CHECK(resolve_title(meeting, Json::object(), clock) == "Meeting 2026-09-19");
    CHECK(format_meeting_date({{"started_at", "2026-03-15T14:30:00"}}, clock) ==
          "2026-03-15 14:30");
    CHECK(has(export_transcript_txt(meeting, Json::object(), {}, clock), "2026-09-19 21:30"));
}

// from
// tests/test_meeting_export_features.py::test_meeting_spanning_legacy_and_utc_timestamps_exports_without_crashing
TEST_CASE("test_meeting_spanning_legacy_and_utc_timestamps_exports_without_crashing",
          "[test_meeting_export_features]") {
    const fv::FakeClock clock(fv::FakeClock::default_utc(), "America/New_York");
    // 09:00 local is 13:00Z (EDT); the end is half an hour later, written in UTC.
    const Json meeting = {{"started_at", "2026-09-19T09:00:00"},
                          {"ended_at", "2026-09-19T13:30:00+00:00"}};
    CHECK(
        has(export_markdown(meeting, Json::object(), {}, true, true, clock), "Duration 00:30:00"));
}

// from
// tests/test_meeting_export_features.py::test_history_export_preserves_every_persisted_field_and_upload_source
TEST_CASE("test_history_export_preserves_every_persisted_field_and_upload_source",
          "[test_meeting_export_features]") {
    fv::HistoryEntry entry;
    entry.id = "2c9f0d7e-0000-4000-8000-000000000001";
    entry.timestamp = "2026-01-01T00:00:00+00:00";
    entry.text = "Cleaned transcript";
    entry.raw_text = "Raw transcript";
    entry.model = "parakeet";
    entry.cleanup_provider = "ollama";
    entry.cleanup_model = "qwen";
    entry.source_name = "Parts 1\xE2\x80\x93"
                        "3.wav";
    entry.audio_file = "recording.wav";
    entry.transcription_time = 1.5;
    entry.audio_duration = 90;
    entry.file_size = 1024;
    const Json serialized = fv::history::serialize_history_entry(entry);
    const Json payload = fv::py::loads(fv::history::render_entry_document(serialized, "json"));
    std::set<std::string> keys;
    for (const auto& [key, value] : payload.items()) {
        keys.insert(key);
    }
    CHECK(keys ==
          std::set<std::string>(fv::history::JSON_FIELDS.begin(), fv::history::JSON_FIELDS.end()));
    const Json expected = {{"id", entry.id},
                           {"timestamp", entry.timestamp},
                           {"model", "parakeet"},
                           {"text", "Cleaned transcript"},
                           {"raw_text", "Raw transcript"},
                           {"cleanup_provider", "ollama"},
                           {"cleanup_model", "qwen"},
                           {"audio_file", "recording.wav"},
                           {"transcription_time", 1.5},
                           {"audio_duration", 90.0},
                           {"file_size", 1024},
                           {"source_name", *entry.source_name}};
    for (const auto& [key, value] : payload.items()) {
        INFO(key);
        CHECK(fv::py::py_equal(value, expected.contains(key) ? expected[key] : Json()));
    }
    for (const auto format : {"markdown", "txt"}) {
        const std::string text = fv::history::render_entry_document(serialized, format);
        CHECK(has(text, *entry.source_name));
        CHECK(has(text, "Cleaned transcript"));
        CHECK(has(text, "Raw transcript"));
        CHECK(has(text, "ollama"));
        CHECK(has(text, "qwen"));
    }
}
