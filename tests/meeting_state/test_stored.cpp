// Loading persisted meetings through meeting/stored.py (tests/test_meeting_stored.py).

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include "meeting_state/segment_ops.h"
#include "meeting_state/stored.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

Json row(const Json& state, const Json& fields = Json::object()) {
    Json out = {{"id", "m_row"},
                {"title", "Row title"},
                {"status", "ended"},
                {"cloud_enabled", true},
                {"state_seq", 9},
                {"state_json", state.is_object() ? Json(fv::py::dumps(state)) : state}};
    for (const auto& [key, value] : fields.items()) {
        out[key] = value;
    }
    return out;
}

Json interrupted_snapshot() {
    MeetingState state;
    state.meeting_id = "m_row";
    state.title = "Snapshot title";
    state.status = "ending";
    state.cloud_enabled = false;
    state.seq = 4;
    Json snapshot = state.to_json();
    snapshot["finalization"] = {{"status", "running"}, {"message", "Writing insights"}};
    snapshot["insight_review"] = {{"status", "running"}, {"questions", Json::array()}};
    snapshot["custom_reports"] = Json::parse(R"([
        {"id": "rp_running", "request": "Budget recap", "status": "running"},
        {"id": "rp_ready", "request": "Owners", "status": "ready", "body": "Done"}])");
    return snapshot;
}

const Json kSettings = Json::object();

} // namespace

// from
// tests/test_meeting_stored.py::test_stored_state_dict_tolerates_missing_and_unreadable_snapshots
TEST_CASE("test_stored_state_dict_tolerates_missing_and_unreadable_snapshots",
          "[test_meeting_stored]") {
    CHECK(stored_state_dict(nullptr) == Json::object());
    CHECK(stored_state_dict({{"state_json", nullptr}}) == Json::object());
    CHECK(stored_state_dict({{"state_json", "{broken"}}) == Json::object());
    CHECK(stored_state_dict({{"state_json", "[1, 2]"}}) == Json::object());
    CHECK(stored_state_dict({{"state_json", R"({"title": "x"})"}}) == Json({{"title", "x"}}));
}

// from
// tests/test_meeting_stored.py::test_historical_load_prefers_the_row_and_stops_interrupted_work
TEST_CASE("test_historical_load_prefers_the_row_and_stops_interrupted_work",
          "[test_meeting_stored]") {
    const MeetingState state = load_state(row(interrupted_snapshot()), true, kSettings);
    CHECK(state.meeting_id == "m_row");
    CHECK(state.title == "Row title");
    CHECK(state.status == "ended");
    CHECK(state.cloud_enabled == true);
    CHECK(state.seq == 4);
    CHECK(state.finalization.status == "failed");
    CHECK(state.insight_review["status"] == "unavailable");
    std::map<std::string, CustomReport> reports;
    for (const auto& report : state.custom_reports) {
        reports[report.id] = report;
    }
    CHECK(reports["rp_running"].status == "failed");
    CHECK(reports["rp_running"].message.find("interrupted") != std::string::npos);
    CHECK(reports["rp_ready"].status == "ready");
}

// from
// tests/test_meeting_stored.py::test_historical_load_of_an_unreadable_snapshot_keeps_row_identity
TEST_CASE("test_historical_load_of_an_unreadable_snapshot_keeps_row_identity",
          "[test_meeting_stored]") {
    const MeetingState state = load_state(row("{broken"), true, kSettings);
    CHECK(state.meeting_id == "m_row");
    CHECK(state.title == "Row title");
    CHECK(state.status == "ended");
    CHECK(state.cloud_enabled == true);
    CHECK(state.seq == 9);
    CHECK(state.finalization.status == "unavailable");
}

// from tests/test_meeting_stored.py::test_working_load_keeps_the_snapshot_as_saved
TEST_CASE("test_working_load_keeps_the_snapshot_as_saved", "[test_meeting_stored]") {
    const MeetingState state = load_state(row(interrupted_snapshot()), "m_row", false, kSettings);
    CHECK(state.title == "Snapshot title");
    CHECK(state.status == "ending");
    CHECK(state.finalization.status == "running");
    CHECK(state.custom_reports[0].status == "running");
}

// from tests/test_meeting_stored.py::test_working_load_of_an_unreadable_snapshot_starts_fresh
TEST_CASE("test_working_load_of_an_unreadable_snapshot_starts_fresh", "[test_meeting_stored]") {
    const MeetingState state = load_state(row("{broken"), "m_row", false, kSettings);
    CHECK(state.meeting_id == "m_row");
    CHECK(state.title == "Row title");
    CHECK(state.cloud_enabled == true);
    CHECK(state.cards["key_points"].empty());
}

// from tests/test_meeting_stored.py::test_historical_store_can_correct_a_speaker
TEST_CASE("test_historical_store_can_correct_a_speaker", "[test_meeting_stored]") {
    // History dashboards apply transcript speaker fixes like a live meeting. The archive
    // dashboard's add_guest and client action are the system upsert and the host op below.
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    f.repo->add_segments({make_segment(meeting_id, "sg_1")});
    MeetingState ended = state_for(meeting_id);
    ended.status = "ended";
    f.repo->update_meeting(meeting_id,
                           {{"status", "ended"}, {"state_json", fv::py::dumps(ended.to_json())}});
    const auto archive =
        open_store(f.repo, meeting_id, *f.repo->get_meeting(meeting_id), true, kSettings, f.clock);
    const auto guest = archive->apply(
        "system", std::nullopt,
        Json::parse(
            R"([{"op": "upsert_participant", "display_name": "Priya", "kind": "guest"}])"))[0];
    REQUIRE(guest.ok);
    const std::string guest_id = guest.effect["participant"]["id"].get<std::string>();

    const auto result = archive->apply("host", std::nullopt,
                                       Json::array({{{"op", "reassign_segment_speaker"},
                                                     {"segment_id", "sg_1"},
                                                     {"participant_id", guest_id}}}))[0];

    INFO(result.reason.value_or(""));
    CHECK(result.ok);
    CHECK(f.repo->get_segment(meeting_id, "sg_1")->at("speaker_participant_id") == guest_id);
    const auto reopened =
        open_store(f.repo, meeting_id, *f.repo->get_meeting(meeting_id), true, kSettings, f.clock);
    CHECK(reopened->snapshot()["participants"].contains(guest_id));
}

TEST_CASE("a store over no repository fails its segment reads instead of crashing",
          "[meeting_state_store]") {
    // Python's helpers over None raise AttributeError from the predicates, and the segment
    // handler logs its failed prior-row read and has no inverse.
    OpResult result;
    result.ok = true;
    result.op = {{"op", "reassign_segment_speaker"}, {"segment_id", "sg_1"}};
    result.effect = {{"entity", "segment_speaker"}, {"segment_id", "sg_1"}};
    CHECK(make_segment_handler(nullptr, "m_none")(result).is_null());

    const auto store = open_store(nullptr, "m_none", {{"id", "m_none"}}, false, kSettings);
    CHECK_THROWS_AS(
        store->apply("agent", "agent", Json::parse(R"([{"op": "add_item", "card": "key_points",
                                     "text": "Cited", "evidence": ["sg_1"]}])")),
        fv::py::PyError);
    CHECK(store->seq() == 0);
    CHECK(store
              ->apply("host", std::nullopt, Json::parse(R"([{"op": "add_item", "card": "key_points",
                           "text": "Uncited"}])"))[0]
              .ok);
}

TEST_CASE("a fresh state takes the report views the settings enable", "[meeting_state_store]") {
    const Json meeting = {{"id", "m_fresh"}, {"title", "Fresh"}, {"cloud_enabled", 1}};
    CHECK(load_state(meeting, false, Json::object()).report_views ==
          std::vector<std::string>{"ribbon", "brief", "signal"});
    CHECK(load_state(meeting, false, {{"meeting_report_ribbon", false}}).report_views ==
          std::vector<std::string>{"brief", "signal"});
    const MeetingState off = load_state(meeting, false,
                                        {{"meeting_report_ribbon", false},
                                         {"meeting_report_brief", false},
                                         {"meeting_report_signal", false}});
    CHECK(off.report_views == std::vector<std::string>{"ribbon"});
    CHECK(off.cloud_enabled);
    CHECK(off.title == "Fresh");
}
