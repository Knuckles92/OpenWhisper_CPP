// Title contracts over one disposable database (three tests of
// tests/test_meeting_title_consistency.py; the dashboard and MCP routes are the web server's
// and the runtime's). The MCP retitle is the repository rename it ends in.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include "meeting_state/patches.h"
#include "meeting_state/stored.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

const Json kSettings = Json::object();

// title_case: an ended meeting with four reports, saved, and a second meeting.
struct TitleCase {
    TitleCase() {
        make_meeting(*f.repo, meeting_id);
        MeetingState state = state_for(meeting_id);
        state.title = "Test meeting";
        state.status = "ended";
        for (int i = 0; i < 4; ++i) {
            CustomReport report;
            report.id = "r_" + std::to_string(i);
            report.request = "Summary";
            report.status = "completed";
            state.custom_reports.push_back(report);
        }
        f.repo->persist_state(meeting_id, state.to_json());
        make_meeting(*f.repo, "m_other");
    }

    std::shared_ptr<MeetingStateStore> open(bool historical = true) {
        return open_store(f.repo, meeting_id, *f.repo->get_meeting(meeting_id), historical,
                          kSettings, f.clock);
    }

    void assert_saved_title(const std::string& title) {
        const Json row = *f.repo->get_meeting(meeting_id);
        CHECK(row["title"] == title);
        CHECK(fv::py::loads(row["state_json"].get<std::string>())["title"] == title);
        CHECK(open()->snapshot()["title"] == title);
    }

    SqlFixture f;
    std::string meeting_id = "m_test1";
};

} // namespace

// from
// tests/test_meeting_title_consistency.py::test_stale_snapshot_cannot_retitle_without_an_explicit_title_op
TEST_CASE("test_stale_snapshot_cannot_retitle_without_an_explicit_title_op",
          "[test_meeting_title_consistency]") {
    const std::string write = GENERATE("op", "runtime", "repository");
    INFO(write);
    TitleCase t;
    const auto stale = t.open();
    t.f.repo->rename_meeting(t.meeting_id, "Canonical title");
    if (write == "op") {
        // remove_custom_report joins the vocabulary with the report module; any op batch on the
        // stale snapshot shows the same contract.
        const Json op = op_domain("remove_custom_report")
                            ? Json{{"op", "remove_custom_report"}, {"report_id", "r_0"}}
                            : Json{{"op", "set_rolling_summary"}, {"text", "Stale summary"}};
        CHECK(stale->apply("host", std::nullopt, Json::array({op}))[0].ok);
        CHECK(stale->snapshot()["title"] == "Canonical title");
    } else if (write == "runtime") {
        CHECK(stale->update_runtime_fields({{"status", "ended"}}));
        CHECK(stale->snapshot()["title"] == "Canonical title");
    } else {
        const Json snapshot = stale->snapshot();
        const Json persisted = t.f.repo->persist_state(t.meeting_id, snapshot);
        t.assert_saved_title("Canonical title");
        CHECK(persisted["title"] == "Canonical title");
        CHECK(snapshot["title"] == "Test meeting");
    }
    t.assert_saved_title("Canonical title");
}

// from
// tests/test_meeting_title_consistency.py::test_explicit_title_op_and_undo_still_update_both_title_fields
TEST_CASE("test_explicit_title_op_and_undo_still_update_both_title_fields",
          "[test_meeting_title_consistency]") {
    TitleCase t;
    const auto store = t.open();
    const auto result = store->apply(
        "host", std::nullopt, Json::parse(R"([{"op": "set_title", "text": "New title"}])"))[0];
    CHECK(result.ok);
    t.assert_saved_title("New title");
    const auto undone = store->undo(*result.seq, std::nullopt);
    REQUIRE_FALSE(undone.empty());
    CHECK(undone[0].ok);
    t.assert_saved_title("Test meeting");
}

// from
// tests/test_meeting_title_consistency.py::test_repository_rejects_invalid_explicit_title_without_partial_audit
TEST_CASE("test_repository_rejects_invalid_explicit_title_without_partial_audit",
          "[test_meeting_title_consistency]") {
    TitleCase t;
    MeetingState stale = state_for(t.meeting_id);
    stale.title = "stale";
    OpResult result;
    result.ok = true;
    result.seq = 1;
    result.op = {{"op", "set_title"}, {"text", std::string(201, 'x')}};
    std::vector<OpResult> results{result};
    CHECK_THROWS_AS(
        t.f.repo->on_ops_applied(t.meeting_id, stale.to_json(), results, "host", std::nullopt),
        fv::py::ValueError);
    CHECK(t.f.repo->list_events(t.meeting_id).empty());
    t.assert_saved_title("Test meeting");
}
