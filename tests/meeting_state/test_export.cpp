// tests/test_meeting_export.py (markdown, JSON and txt shape, token stripping), the export tests
// of tests/test_meeting_intent.py (TestIntentExport) and of tests/test_meeting_notes_agent.py
// (TestNotesExport).

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "base/testing/fake_clock.h"
#include "export_fixture.h"
#include "meeting_state/export_json.h"
#include "meeting_state/export_markdown.h"
#include "meeting_state/export_txt.h"
#include "meeting_state/schema.h"
#include "pyport/json.h"

using fv::Json;
using namespace fv::meeting_state;
using fv::meeting_state::test_support::ExportFixture;

namespace {

bool has(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

const std::string kBrief = "We decide the vendor today. Capture who objected and why, and the "
                           "number attached to each bid.";

Json state_with_brief() {
    MeetingState state;
    state.meeting_id = "m_intent";
    state.intent.text = kBrief;
    return state.to_json();
}

Json intent_meeting() {
    return {{"id", "m_intent"},
            {"title", "Vendor review"},
            {"status", "ended"},
            {"started_at", "2026-03-15T14:30:00"},
            {"ended_at", "2026-03-15T15:00:00"}};
}

} // namespace

// from tests/test_meeting_export.py::TestJsonExport::test_strips_tokens_and_volatile_fields
TEST_CASE("TestJsonExport::test_strips_tokens_and_volatile_fields", "[test_meeting_export]") {
    const ExportFixture f;
    const std::string raw = export_json(f.meeting, f.state, f.segments);
    const Json data = fv::py::loads(raw);
    CHECK(data["format_version"] == EXPORT_FORMAT_VERSION);
    CHECK(data["meeting"]["id"] == "m_export");
    CHECK(data["meeting"]["title"] == "Planning Sync");
    for (const char* key :
         {"host_token", "guest_token", "app_pid", "app_heartbeat_at", "state_json"}) {
        CHECK_FALSE(data["meeting"].contains(key));
    }
    CHECK_FALSE(has(raw, "SECRET_HOST_TOKEN_XXXX"));
    CHECK_FALSE(has(raw, "SECRET_GUEST_TOKEN_YYYY"));
    CHECK(data["state"]["topic"]["current"] == "Q3 roadmap");
    CHECK(data["segments"].size() == 2);
    CHECK(data["segments"][0]["text"] == "Welcome everyone");
}

// from tests/test_meeting_export.py::TestJsonExport::test_preserves_evidence_ids
TEST_CASE("TestJsonExport::test_preserves_evidence_ids", "[test_meeting_export]") {
    const ExportFixture f;
    const Json data = fv::py::loads(export_json(f.meeting, f.state, f.segments));
    CHECK(data["state"]["cards"]["key_points"][0]["evidence"] == Json::array({"sg_1"}));
}

// from tests/test_meeting_export.py::TestMarkdownExport::test_golden_structure
TEST_CASE("TestMarkdownExport::test_golden_structure", "[test_meeting_export]") {
    const ExportFixture f;
    const std::string md = export_markdown(f.meeting, f.state, f.segments, true, true, f.clock);
    CHECK(md.starts_with("# Planning Sync\n"));
    for (const char* text : {"2026-03-15 14:30",
                             "Participants: Alex, Sam",
                             "## Topic",
                             "Q3 roadmap",
                             "## Summary",
                             "Team aligned on milestones.",
                             "## Key Points",
                             "- [00:05] Ship beta in June",
                             "## Decisions",
                             "- Use FastAPI",
                             "## Action Items",
                             "**Mine**",
                             "- Draft RFC",
                             "## Risks",
                             "**[high]**",
                             "## Timeline",
                             "[01:30]",
                             "## Questions",
                             "**Open:** Deadline firm?",
                             "**Resolved:** Who owns RFC?",
                             "answered from audio",
                             "### Transcript",
                             "[00:00:05] Alex: Welcome everyone",
                             "[00:00:10] Sam: Let's plan Q3"}) {
        INFO(text);
        CHECK(has(md, text));
    }
    CHECK_FALSE(has(md, "Ignored")); // dismissed omitted
    CHECK_FALSE(has(md, "scratch")); // removed notes omitted
    CHECK(md.ends_with("\n"));
}

// from tests/test_meeting_export.py::TestMarkdownExport::test_can_omit_transcript_and_intelligence
TEST_CASE("TestMarkdownExport::test_can_omit_transcript_and_intelligence",
          "[test_meeting_export]") {
    const ExportFixture f;
    const std::string md = export_markdown(f.meeting, f.state, f.segments, false, false, f.clock);
    CHECK(md.starts_with("# Planning Sync\n"));
    for (const char* text : {"## Key Points", "## Summary", "### Transcript", "Welcome everyone",
                             "Ship beta in June"}) {
        INFO(text);
        CHECK_FALSE(has(md, text));
    }
    CHECK(has(md, "2026-03-15 14:30"));
}

// from tests/test_meeting_export.py::TestTranscriptTxtExport::test_plain_transcript
TEST_CASE("TestTranscriptTxtExport::test_plain_transcript", "[test_meeting_export]") {
    const ExportFixture f;
    const std::string txt = export_transcript_txt(f.meeting, f.state, f.segments, f.clock);
    CHECK(txt.starts_with("Planning Sync\n"));
    CHECK(has(txt, "2026-03-15 14:30"));
    CHECK(has(txt, "Participants: Alex, Sam"));
    CHECK(has(txt, "[00:00:05] Alex: Welcome everyone"));
    CHECK(has(txt, "[00:00:10] Sam: Let's plan Q3"));
    CHECK_FALSE(has(txt, "SECRET_HOST"));
    CHECK(txt.ends_with("\n"));
}

// from tests/test_meeting_export.py::TestTranscriptTxtExport::test_empty_segments_placeholder
TEST_CASE("TestTranscriptTxtExport::test_empty_segments_placeholder", "[test_meeting_export]") {
    const ExportFixture f;
    CHECK(has(export_transcript_txt(f.meeting, f.state, {}, f.clock), "(no transcript)"));
}

// from tests/test_meeting_intent.py::TestIntentExport::test_the_brief_heads_the_exported_document
TEST_CASE("TestIntentExport::test_the_brief_heads_the_exported_document", "[test_meeting_intent]") {
    const std::string md = export_markdown(intent_meeting(), state_with_brief(), {});
    CHECK(has(md, "## Meeting Brief"));
    CHECK(has(md, "> " + kBrief));
}

// from tests/test_meeting_intent.py::TestIntentExport::test_an_unset_brief_adds_no_empty_section
TEST_CASE("TestIntentExport::test_an_unset_brief_adds_no_empty_section", "[test_meeting_intent]") {
    MeetingState state;
    state.meeting_id = "m_intent";
    CHECK_FALSE(has(export_markdown(intent_meeting(), state.to_json(), {}), "Meeting Brief"));
}

// from
// tests/test_meeting_intent.py::TestIntentExport::test_the_brief_survives_an_intelligence_free_export
TEST_CASE("TestIntentExport::test_the_brief_survives_an_intelligence_free_export",
          "[test_meeting_intent]") {
    CHECK(
        has(export_markdown(intent_meeting(), state_with_brief(), {}, true, false), "> " + kBrief));
}

// from
// tests/test_meeting_notes_agent.py::TestNotesExport::test_markdown_export_renders_meeting_notes_section
TEST_CASE("TestNotesExport::test_markdown_export_renders_meeting_notes_section",
          "[test_meeting_notes_agent]") {
    const Json state = Json::parse(R"({"participants": {},
        "topic": {"current": "Funnel review", "history": []},
        "rolling_summary": "Team reviewed onboarding.",
        "cards": {"live_notes": [
            {"status": "proposed", "text": "Maria walked the funnel.",
             "data": {"heading": "Funnel review", "start_s": 41.0}, "evidence": []},
            {"status": "proposed", "text": "Team agreed to try OAuth.",
             "data": {"heading": "Login decision"}, "evidence": []}]},
        "questions": []})");
    const std::string md = export_markdown(
        {{"id", "m1"}, {"title", "Weekly"}, {"started_at", nullptr}, {"status", "ended"}}, state,
        {});
    CHECK(has(md, "## Meeting Notes"));
    CHECK(has(md, "**00:41 Funnel review** \xE2\x80\x94 Maria walked the funnel."));
    CHECK(has(md, "**Login decision** \xE2\x80\x94 Team agreed to try OAuth."));
}

// from tests/test_meeting_notes_agent.py::TestNotesExport::test_markdown_export_skips_empty_notes
TEST_CASE("TestNotesExport::test_markdown_export_skips_empty_notes", "[test_meeting_notes_agent]") {
    const Json state = Json::parse(R"({"participants": {},
        "topic": {"current": "", "history": []}, "rolling_summary": "", "cards": {},
        "questions": []})");
    CHECK_FALSE(
        has(export_markdown(
                {{"id", "m1"}, {"title", "Weekly"}, {"started_at", nullptr}, {"status", "ended"}},
                state, {}),
            "Meeting Notes"));
}

TEST_CASE("markdown: the exact document of the export fixture", "[meeting_state_export]") {
    const ExportFixture f;
    const std::string md = export_markdown(f.meeting, f.state, f.segments, true, true, f.clock);
    CHECK(md == "# Planning Sync\n"
                "\n"
                "2026-03-15 14:30 \xC2\xB7 Duration 00:00:14 \xC2\xB7 Participants: Alex, Sam\n"
                "\n"
                "## Topic\n"
                "\n"
                "Q3 roadmap\n"
                "\n"
                "Previously:\n"
                "\n"
                "- Kickoff (14:30)\n"
                "\n"
                "## Summary\n"
                "\n"
                "Team aligned on milestones.\n"
                "\n"
                "## Key Points\n"
                "\n"
                "- [00:05] Ship beta in June\n"
                "\n"
                "## Decisions\n"
                "\n"
                "- Use FastAPI\n"
                "\n"
                "## Action Items\n"
                "\n"
                "**Mine**\n"
                "\n"
                "- Draft RFC\n"
                "\n"
                "## Risks\n"
                "\n"
                "- **[high]** Vendor delay\n"
                "\n"
                "## Timeline\n"
                "\n"
                "- [01:30] Kickoff done\n"
                "\n"
                "## Questions\n"
                "\n"
                "- **Open:** Deadline firm?\n"
                "  - Suggested answer (60% confidence): Yes, June 30\n"
                "- **Resolved:** Who owns RFC?\n"
                "  - Answer: Alex \xE2\x80\x94 _answered from audio_\n"
                "\n"
                "### Transcript\n"
                "\n"
                "[00:00:05] Alex: Welcome everyone  \n"
                "[00:00:10] Sam: Let's plan Q3  \n");
}

TEST_CASE("txt: the formatting helpers", "[meeting_state_export]") {
    CHECK(format_clock(-5) == "00:00:00");
    CHECK(format_clock(3725.9) == "01:02:05");
    CHECK(format_clock(360000) == "100:00:00");
    CHECK(format_mmss(3725.9) == "62:05");
    CHECK_THROWS(format_clock(std::numeric_limits<double>::quiet_NaN()));
    const fv::FakeClock clock(fv::FakeClock::default_utc(), "America/New_York");
    CHECK(resolve_title({{"title", "  "}}, {{"title", " State "}}, clock) == "State");
    CHECK(resolve_title({{"started_at", "2026-03-15T03:30:00Z"}}, Json::object(), clock) ==
          "Meeting 2026-03-14");
    CHECK(resolve_title(Json::object(), Json::object(), clock) == "Meeting");
    CHECK(format_meeting_date({{"started_at", "not a date"}}, clock) == "not a date");
    CHECK_FALSE(format_meeting_date(Json::object(), clock).has_value());
    CHECK(speaker_name({{"speaker_participant_id", "p_x"}, {"channel", "loopback"}},
                       Json::object()) == "Others");
}
