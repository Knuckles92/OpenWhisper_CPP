// tests/test_meeting_custom_report.py: the custom-report state ops (TestOps) and the Markdown
// export of reports (TestExport). The report worker, corpus and REST routes are M8's and M7.16's;
// the report fan-out is M7.13's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "meeting_state/custom_report_ops.h"
#include "meeting_state/export_markdown.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"

using fv::Json;
using namespace fv::meeting_state;

namespace {

constexpr const char* kPastId = "m_past";

Json segment(int index, double start_s, const std::string& text,
             const std::string& channel = "mic") {
    return {{"id", std::format("sg_{}", index)},
            {"meeting_id", kPastId},
            {"chunk_id", 1},
            {"channel", channel},
            {"start_s", start_s},
            {"end_s", start_s + 4.0},
            {"text", text},
            {"speaker_participant_id", nullptr},
            {"speaker_source", "channel"},
            {"speaker_pinned", false}};
}

const std::vector<Json>& segments() {
    static const std::vector<Json> rows = {
        segment(1, 0, "Lets talk about the vendor renewal and pricing."),
        segment(2, 6, "We cannot go below a twelve percent discount.", "loopback"),
        segment(3, 14, "Priya will draft the renewal note by Friday."),
        segment(4, 24, "Agreed, we hold at twelve and revisit the terms in Q3."),
    };
    return rows;
}

const Json& meeting() {
    static const Json row = {{"id", kPastId},
                             {"title", "Vendor renewal"},
                             {"started_at", "2026-09-19T10:00:00Z"},
                             {"cloud_enabled", true}};
    return row;
}

MeetingState ended_state() {
    MeetingState state;
    state.meeting_id = kPastId;
    state.title = "Vendor renewal";
    state.status = "ended";
    state.cloud_enabled = true;
    state.finalization = FinalizationState::coerce(Json{{"status", "completed"}}, true, "ended");
    return state;
}

struct Fixture {
    fv::FakeClock clock;
    testing::FixedIds ids;
    MeetingStateStore store;

    explicit Fixture(MeetingState state = ended_state())
        : store(std::move(state), nullptr, {}, clock, ids) {}

    OpResult request(const std::string& actor = "host", Json overrides = Json::object()) {
        Json op = {
            {"op", "request_custom_report"}, {"request", "the commitments"}, {"run_id", "run_1"}};
        for (auto& [key, value] : overrides.items()) {
            op[key] = value;
        }
        return store.apply(actor, std::nullopt, Json::array({op}))[0];
    }

    OpResult publish(const std::string& actor_type, const std::optional<std::string>& actor_id,
                     const Json& op) {
        return store.apply(actor_type, actor_id, Json::array({op}))[0];
    }
};

Json finish(const std::string& report_id, Json extra = Json::object()) {
    Json op = {{"op", "finish_custom_report"},
               {"report_id", report_id},
               {"run_id", "run_1"},
               {"status", "ready"},
               {"markdown", "# x"}};
    for (auto& [key, value] : extra.items()) {
        op[key] = value;
    }
    return op;
}

const std::string kWorker(WORKER_ACTOR);

} // namespace

// from tests/test_meeting_custom_report.py::TestOps::test_host_asks_and_only_the_worker_publishes
TEST_CASE("TestOps::test_host_asks_and_only_the_worker_publishes", "[test_meeting_custom_report]") {
    Fixture f;
    const auto claim = f.request();
    REQUIRE(claim.ok);
    CHECK(claim.effect["entity"] == "custom_report");
    const std::string report_id = *claim.target_id;
    CHECK(f.store.snapshot()["custom_reports"][0]["status"] == "running");
    const Json op = finish(report_id, {{"markdown", "# R\n\nbody"}});
    // Neither a guest nor the agent may write report text.
    CHECK(f.publish("user", "p_1", op).reason == "system_only");
    CHECK(f.publish("agent", "agent", op).reason == "agent_forbidden");
    CHECK(f.publish("system", "other", op).reason == "system_only");
    CHECK(f.publish("system", kWorker, op).ok);
    const Json report = f.store.snapshot()["custom_reports"][0];
    CHECK(report["status"] == "ready");
    CHECK(report["markdown"].get<std::string>().starts_with("# R"));
}

// from
// tests/test_meeting_custom_report.py::TestOps::test_guests_cannot_ask_and_the_agent_cannot_either
TEST_CASE("TestOps::test_guests_cannot_ask_and_the_agent_cannot_either",
          "[test_meeting_custom_report]") {
    Fixture f;
    CHECK(f.request("user").reason == "host_only");
    CHECK(f.request("agent").reason == "agent_forbidden");
    CHECK(f.store.snapshot()["custom_reports"].empty());
}

// from tests/test_meeting_custom_report.py::TestOps::test_request_validation
TEST_CASE("TestOps::test_request_validation", "[test_meeting_custom_report]") {
    const auto [overrides, reason] = GENERATE(table<Json, std::string>({
        {Json{{"request", ""}}, "invalid_request"},
        {Json{{"request", "   "}}, "invalid_request"},
        {Json{{"request", 42}}, "invalid_request"},
        {Json{{"request", std::string(MAX_REQUEST_CHARS + 1, 'x')}}, "request_too_long"},
        {Json{{"run_id", ""}}, "invalid_run_id"},
    }));
    Fixture f;
    CHECK(f.request("host", overrides).reason == reason);
}

// from tests/test_meeting_custom_report.py::TestOps::test_reports_wait_for_a_finished_meeting
TEST_CASE("TestOps::test_reports_wait_for_a_finished_meeting", "[test_meeting_custom_report]") {
    MeetingState live;
    live.meeting_id = kPastId;
    live.status = "active";
    Fixture active(live);
    CHECK(active.request().reason == "meeting_not_ready");
    MeetingState state = ended_state();
    state.finalization = FinalizationState::coerce(Json{{"status", "running"}}, true, "ended");
    Fixture finalizing(state);
    CHECK(finalizing.request().reason == "meeting_not_ready");
}

// from tests/test_meeting_custom_report.py::TestOps::test_only_one_report_runs_at_a_time
TEST_CASE("TestOps::test_only_one_report_runs_at_a_time", "[test_meeting_custom_report]") {
    Fixture f;
    CHECK(f.request().ok);
    CHECK(f.request("host", {{"run_id", "run_2"}}).reason == "report_running");
}

// from tests/test_meeting_custom_report.py::TestOps::test_the_cap_bounds_what_one_meeting_stores
TEST_CASE("TestOps::test_the_cap_bounds_what_one_meeting_stores", "[test_meeting_custom_report]") {
    Fixture f;
    for (std::size_t index = 0; index < MAX_CUSTOM_REPORTS; ++index) {
        const std::string run = std::format("run_{}", index);
        const auto claim = f.request("host", {{"run_id", run}});
        REQUIRE(claim.ok);
        f.publish("system", kWorker, finish(*claim.target_id, {{"run_id", run}}));
    }
    CHECK(f.request("host", {{"run_id", "run_last"}}).reason == "report_limit_reached");
}

// from
// tests/test_meeting_custom_report.py::TestOps::test_a_stale_run_cannot_overwrite_a_finished_report
TEST_CASE("TestOps::test_a_stale_run_cannot_overwrite_a_finished_report",
          "[test_meeting_custom_report]") {
    Fixture f;
    const std::string report_id = *f.request().target_id;
    const Json op = finish(report_id, {{"markdown", "# first"}});
    CHECK(f.publish("system", kWorker, op).ok);
    // Same id, same run, already finished.
    CHECK(f.publish("system", kWorker, op).reason == "stale_report");
    // Same id, a different run that lost the race.
    CHECK(f.publish("system", kWorker,
                    finish(report_id, {{"run_id", "run_2"}, {"markdown", "# second"}}))
              .reason == "stale_report");
    CHECK(f.store.snapshot()["custom_reports"][0]["markdown"] == "# first");
}

// from tests/test_meeting_custom_report.py::TestOps::test_finish_validation
TEST_CASE("TestOps::test_finish_validation", "[test_meeting_custom_report]") {
    const auto [overrides, reason] = GENERATE(table<Json, std::string>({
        {Json{{"status", "running"}}, "invalid_report_status"},
        {Json{{"status", "bogus"}}, "invalid_report_status"},
        {Json{{"status", "ready"}, {"markdown", "   "}}, "empty_report"},
        {Json{{"report_id", "rep_missing"}}, "unknown_report"},
    }));
    Fixture f;
    const std::string report_id = *f.request().target_id;
    CHECK(f.publish("system", kWorker, finish(report_id, overrides)).reason == reason);
}

// from
// tests/test_meeting_custom_report.py::TestOps::test_a_failed_report_keeps_its_message_and_frees_the_slot
TEST_CASE("TestOps::test_a_failed_report_keeps_its_message_and_frees_the_slot",
          "[test_meeting_custom_report]") {
    Fixture f;
    const std::string report_id = *f.request().target_id;
    Json op = {{"op", "finish_custom_report"},
               {"report_id", report_id},
               {"run_id", "run_1"},
               {"status", "failed"},
               {"message", "No API key."}};
    CHECK(f.publish("system", kWorker, op).ok);
    const Json report = f.store.snapshot()["custom_reports"][0];
    CHECK(report["status"] == "failed");
    CHECK(report["message"] == "No API key.");
    CHECK(f.request("host", {{"run_id", "run_2"}}).ok);
}

// from
// tests/test_meeting_custom_report.py::TestOps::test_discarding_a_running_report_unblocks_the_meeting
TEST_CASE("TestOps::test_discarding_a_running_report_unblocks_the_meeting",
          "[test_meeting_custom_report]") {
    // The escape hatch for a run stranded by a crash.
    Fixture f;
    const std::string report_id = *f.request().target_id;
    const auto removal =
        f.publish("host", std::nullopt, {{"op", "remove_custom_report"}, {"report_id", report_id}});
    CHECK(removal.ok);
    CHECK(removal.effect["removed"] == true);
    CHECK(f.store.snapshot()["custom_reports"].empty());
    // The abandoned worker's finish lands on nothing, and a retry works.
    CHECK(f.publish("system", kWorker, finish(report_id, {{"markdown", "# late"}})).reason ==
          "unknown_report");
    CHECK(f.request("host", {{"run_id", "run_2"}}).ok);
}

// from tests/test_meeting_custom_report.py::TestOps::test_guests_cannot_delete_a_report
TEST_CASE("TestOps::test_guests_cannot_delete_a_report", "[test_meeting_custom_report]") {
    Fixture f;
    const std::string report_id = *f.request().target_id;
    CHECK(f.publish("user", "p_1", {{"op", "remove_custom_report"}, {"report_id", report_id}})
              .reason == "host_only");
}

// from
// tests/test_meeting_custom_report.py::TestOps::test_reports_round_trip_through_the_serialization_contract
TEST_CASE("TestOps::test_reports_round_trip_through_the_serialization_contract",
          "[test_meeting_custom_report]") {
    Fixture f;
    const std::string report_id = *f.request().target_id;
    f.publish(
        "system", kWorker,
        finish(report_id,
               {{"markdown", "# R"}, {"title", "R"}, {"sources", {{"transcript_lines", 4}}}}));
    const Json snapshot = f.store.snapshot();
    CHECK(MeetingState::from_json(snapshot, f.clock).to_json() == snapshot);
    // A legacy snapshot without the key restores as an empty list.
    Json legacy = snapshot;
    legacy.erase("custom_reports");
    CHECK(MeetingState::from_json(legacy, f.clock).to_json()["custom_reports"] == Json::array());
}

// from
// tests/test_meeting_custom_report.py::TestExport::test_finished_reports_join_the_markdown_export
TEST_CASE("TestExport::test_finished_reports_join_the_markdown_export",
          "[test_meeting_custom_report]") {
    Fixture f;
    const std::pair<std::string, std::string> runs[] = {
        {"ready", "# Commitments\n\n## Owners\n\n- Priya by Friday"}, {"failed", ""}};
    for (std::size_t index = 0; index < 2; ++index) {
        const auto& [status, markdown] = runs[index];
        const std::string run = std::format("run_{}", index);
        const std::string report_id = *f.request("host", {{"run_id", run}}).target_id;
        f.publish("system", kWorker,
                  {{"op", "finish_custom_report"},
                   {"report_id", report_id},
                   {"run_id", run},
                   {"status", status},
                   {"markdown", markdown},
                   {"title", markdown.empty() ? "" : "Commitments"},
                   {"message", markdown.empty() ? "failed" : ""}});
    }
    const std::string document =
        export_markdown(meeting(), f.store.snapshot(), segments(), true, true, f.clock);
    CHECK(document.find("## Requested Reports") != std::string::npos);
    CHECK(document.find("### Commitments") != std::string::npos);
    CHECK(document.find("> Requested: the commitments") != std::string::npos);
    // The report's own headings nest under the section instead of colliding.
    CHECK(document.find("##### Owners") != std::string::npos);
    // Its opening "# Commitments" is dropped: the section heading says it.
    std::size_t count = 0;
    for (std::size_t at = document.find("Commitments"); at != std::string::npos;
         at = document.find("Commitments", at + 1)) {
        ++count;
    }
    CHECK(count == 1);
    CHECK(document.find("#### Commitments") == std::string::npos);
}

// from
// tests/test_meeting_custom_report.py::TestExport::test_a_meeting_without_reports_gains_no_section
TEST_CASE("TestExport::test_a_meeting_without_reports_gains_no_section",
          "[test_meeting_custom_report]") {
    const std::string document =
        export_markdown(meeting(), ended_state().to_json(), segments(), true, true);
    CHECK(document.find("Requested Reports") == std::string::npos);
}

// from
// tests/test_meeting_custom_report.py::TestExport::test_heading_demotion_skips_fenced_code_and_caps_at_six
TEST_CASE("TestExport::test_heading_demotion_skips_fenced_code_and_caps_at_six",
          "[test_meeting_custom_report]") {
    const std::string demoted = demote_headings("# a\n\n```\n# not a heading\n```\n\n###### deep");
    CHECK(demoted.starts_with("#### a"));
    CHECK(demoted.find("# not a heading") != std::string::npos);
    CHECK(demoted.ends_with("###### deep"));
}

TEST_CASE("custom reports: clipping, sources and system requests", "[meeting_state_reports]") {
    Fixture f;
    const auto claim = f.request("system", {{"request", "  Pricing  "}});
    REQUIRE(claim.ok);
    CHECK(claim.effect["report"]["request"] == "Pricing");
    CHECK(claim.effect["report"]["message"] ==
          "Reading the meeting and writing your report\xE2\x80\xA6");
    CHECK(claim.effect["removed"] == false);
    CHECK(claim.inverse.is_null());
    const auto done =
        f.publish("system", kWorker,
                  finish(*claim.target_id, {{"markdown", std::string(MAX_REPORT_CHARS + 10, 'm')},
                                            {"title", "  " + std::string(MAX_TITLE_CHARS + 5, 't')},
                                            {"message", std::string(MAX_MESSAGE_CHARS + 5, 'g')},
                                            {"sources", Json::array({1})}}));
    REQUIRE(done.ok);
    const Json report = f.store.snapshot()["custom_reports"][0];
    CHECK(report["markdown"].get<std::string>().size() == MAX_REPORT_CHARS);
    // Clipped first, then stripped: the leading spaces count against the limit.
    CHECK(report["title"].get<std::string>().size() == MAX_TITLE_CHARS - 2);
    CHECK(report["message"].get<std::string>().size() == MAX_MESSAGE_CHARS);
    CHECK(report["sources"] == Json::object());
}
