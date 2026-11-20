// Golden vectors of meeting.state.review, meeting.state.custom_reports, meeting.fast_state,
// meeting.state.repair and meeting.export.* (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "golden/golden.h"
#include "meeting_state/export_bulk.h"
#include "meeting_state/export_json.h"
#include "meeting_state/export_markdown.h"
#include "meeting_state/export_txt.h"
#include "meeting_state/patches.h"
#include "meeting_state/repair.h"
#include "meeting_state/review_ops.h"
#include "meeting_state/schema.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fixed_ids.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "testkit/testkit.h"

namespace golden = fv::golden;
using fv::Json;
using namespace fv::meeting_state;

namespace {

// The Python exception's class name, or "" when nothing pyport-shaped was thrown.
template <class Fn> std::string raised(Fn&& fn) {
    try {
        fn();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name());
    }
    return "";
}

template <class Fn> void replay(const golden::Case& c, Fn&& fn) {
    INFO(c.label());
    if (c.raises()) {
        CHECK(raised(fn) == c.raises_type());
        return;
    }
    const Json actual = fn();
    INFO("actual " << actual.dump());
    CHECK(golden::equal(c.result<Json>(), actual));
}

// The state ops pin now at FakeClock's default, 2026-01-01T00:00:00Z.
const fv::FakeClock& utc_clock() {
    static const fv::FakeClock clock;
    return clock;
}

// The exports pin the zone to America/New_York and now at 2026-01-01T00:00:00Z.
const fv::FakeClock& new_york_clock() {
    static const fv::FakeClock clock(fv::FakeClock::default_utc(), "America/New_York");
    return clock;
}

std::function<bool(std::string_view)> predicate(const Json& ids) {
    if (ids.is_null()) {
        return {};
    }
    std::set<std::string, std::less<>> known;
    for (const auto& id : ids) {
        known.insert(id.get<std::string>());
    }
    return [known](std::string_view segment_id) {
        return known.contains(segment_id);
    };
}

MeetingState initial_state(const Json& initial) {
    if (initial.is_null()) {
        MeetingState state;
        state.meeting_id = "m_test";
        return state;
    }
    return MeetingState::from_json(initial, utc_clock());
}

// script(initial, batches) of the op-script modules: apply_ops batch by batch.
Json run_script(const golden::Case& c) {
    testing::FixedIds ids;
    MeetingState state = initial_state(c.args()[0]);
    Json results = Json::array();
    for (const auto& batch : c.args()[1]) {
        OpContext ctx;
        ctx.actor_type = batch.at(0).get<std::string>();
        if (!batch.at(1).is_null()) {
            ctx.actor_id = batch.at(1).get<std::string>();
        }
        ctx.segment_exists = predicate(batch.at(2));
        ctx.segment_pinned = predicate(batch.at(3));
        ctx.clock = &utc_clock();
        ctx.ids = &ids;
        Json batch_results = Json::array();
        for (const auto& result : apply_ops(state, ctx, batch.at(4))) {
            batch_results.push_back(result.to_json());
        }
        results.push_back(std::move(batch_results));
    }
    return Json{{"results", std::move(results)}, {"state", state.to_json()}};
}

std::vector<Json> rows(const Json& list) {
    std::vector<Json> out;
    for (const auto& row : list) {
        out.push_back(row);
    }
    return out;
}

Json optional_text(const std::optional<std::string>& text) {
    return text ? Json(*text) : Json();
}

Json strings(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

MeetingExport entry_of(const Json& entry) {
    MeetingExport out;
    out.meeting = entry.at("meeting");
    out.state = entry.at("state");
    out.segments = rows(entry.at("segments"));
    return out;
}

std::optional<fv::py::DateTime> bound(const Json& iso) {
    if (iso.is_null()) {
        return std::nullopt;
    }
    return fv::py::fromisoformat(iso.get<std::string>());
}

double seconds(const Json& value) {
    return *fv::py::py_float(value);
}

} // namespace

TEST_CASE("golden meeting.state.review", "[golden]") {
    golden::for_each("meeting.state.review", "script",
                     [&](const golden::Case& c) { replay(c, [&] { return run_script(c); }); });
    golden::for_each("meeting.state.review", "invalidate_checks", [&](const golden::Case& c) {
        replay(c, [&] {
            MeetingState state = initial_state(c.args()[0]);
            invalidate_checks(state);
            return state.to_json();
        });
    });
}

TEST_CASE("golden meeting.state.custom_reports", "[golden]") {
    golden::for_each("meeting.state.custom_reports", "script",
                     [&](const golden::Case& c) { replay(c, [&] { return run_script(c); }); });
}

TEST_CASE("golden meeting.fast_state", "[golden]") {
    golden::for_each("meeting.fast_state", "script",
                     [&](const golden::Case& c) { replay(c, [&] { return run_script(c); }); });
}

TEST_CASE("golden meeting.state.repair", "[golden]") {
    const auto builder = [](const char* name, auto build) {
        golden::for_each("meeting.state.repair", name, [&](const golden::Case& c) {
            replay(c, [&] { return build(c.args()[0], rows(c.args()[1])); });
        });
    };
    builder("build_timeline_backfill_ops", build_timeline_backfill_ops);
    builder("build_timeline_coverage_from_segments", build_timeline_coverage_from_segments);
    builder("build_topic_backfill_ops", build_topic_backfill_ops);
    builder("build_summary_backfill_ops", build_summary_backfill_ops);
    golden::for_each("meeting.state.repair", "repair", [&](const golden::Case& c) {
        replay(c, [&] {
            testing::FixedIds ids;
            MeetingStateStore store(MeetingState::from_json(c.args()[0], utc_clock()), nullptr, {},
                                    utc_clock(), ids);
            const int applied = repair_meeting_state(store, rows(c.args()[1]));
            return Json{{"applied", applied}, {"state", store.snapshot()}};
        });
    });
}

TEST_CASE("golden meeting.export.markdown", "[golden]") {
    const auto& clock = new_york_clock();
    golden::for_each("meeting.export.markdown", "export_markdown", [&](const golden::Case& c) {
        replay(c, [&] {
            return Json(export_markdown(c.args()[0], c.args()[1], rows(c.args()[2]), c.arg<bool>(3),
                                        c.arg<bool>(4), clock));
        });
    });
    golden::for_each("meeting.export.markdown", "demote_headings", [&](const golden::Case& c) {
        replay(c, [&] { return Json(demote_headings(c.arg<std::string>(0), c.arg<int>(1))); });
    });
}

TEST_CASE("golden meeting.export.json_export", "[golden]") {
    golden::for_each("meeting.export.json_export", "export_json", [&](const golden::Case& c) {
        replay(c, [&] { return Json(export_json(c.args()[0], c.args()[1], rows(c.args()[2]))); });
    });
}

TEST_CASE("golden meeting.export.transcript_txt", "[golden]") {
    const auto& clock = new_york_clock();
    const char* module = "meeting.export.transcript_txt";
    golden::for_each(module, "format_clock", [&](const golden::Case& c) {
        replay(c, [&] { return Json(format_clock(seconds(c.args()[0]))); });
    });
    golden::for_each(module, "format_mmss", [&](const golden::Case& c) {
        replay(c, [&] { return Json(format_mmss(seconds(c.args()[0]))); });
    });
    golden::for_each(module, "resolve_title", [&](const golden::Case& c) {
        replay(c, [&] { return Json(resolve_title(c.args()[0], c.args()[1], clock)); });
    });
    golden::for_each(module, "format_meeting_date", [&](const golden::Case& c) {
        replay(c, [&] { return optional_text(format_meeting_date(c.args()[0], clock)); });
    });
    golden::for_each(module, "participant_names", [&](const golden::Case& c) {
        replay(c, [&] { return strings(participant_names(c.args()[0])); });
    });
    golden::for_each(module, "speaker_name", [&](const golden::Case& c) {
        replay(c, [&] { return Json(speaker_name(c.args()[0], c.args()[1])); });
    });
    golden::for_each(module, "transcript_lines", [&](const golden::Case& c) {
        replay(c, [&] { return strings(transcript_lines(rows(c.args()[0]), c.args()[1])); });
    });
    golden::for_each(module, "export_transcript_txt", [&](const golden::Case& c) {
        replay(c, [&] {
            return Json(export_transcript_txt(c.args()[0], c.args()[1], rows(c.args()[2]), clock));
        });
    });
}

TEST_CASE("golden meeting.export.bulk", "[golden]") {
    const auto& clock = new_york_clock();
    const char* module = "meeting.export.bulk";
    golden::for_each(module, "meeting_file_stem", [&](const golden::Case& c) {
        replay(c, [&] { return Json(meeting_file_stem(c.args()[0], c.args()[1], clock)); });
    });
    golden::for_each(module, "filter_export_meetings", [&](const golden::Case& c) {
        replay(c, [&] {
            Json ids = Json::array();
            for (const auto& meeting :
                 filter_export_meetings(rows(c.args()[0]), bound(c.args()[1]), bound(c.args()[2]),
                                        c.arg<bool>(3), clock)) {
                ids.push_back(meeting.value("id", Json()));
            }
            return ids;
        });
    });
    golden::for_each(module, "render_meeting_document", [&](const golden::Case& c) {
        replay(c, [&] {
            return Json(render_meeting_document(entry_of(c.args()[0]), c.arg<std::string>(1),
                                                {c.arg<bool>(2), c.arg<bool>(3)}, clock));
        });
    });
    // The acceptance: the fixture database OpenWhisper made (M7.7's) exports byte for byte as
    // OpenWhisper exports it, through this repository's reads.
    const fv::testkit::TempDataRoot root;
    const auto fixture = fv::testkit::source_root() / "tests" / "fixtures" / "meeting_state" /
                         "openwhisper-meetings.db";
    std::filesystem::copy_file(fixture, root.path() / "fixture.db");
    fv::Database database = fv::Database::open_or_create(root.path() / "fixture.db");
    SqlMeetingRepository repository(database);
    golden::for_each(module, "fixture_export", [&](const golden::Case& c) {
        replay(c, [&] {
            const auto entry = collect_meeting_export(repository, c.arg<std::string>(0));
            if (!entry) {
                return Json();
            }
            return Json(render_meeting_document(*entry, c.arg<std::string>(1),
                                                {c.arg<bool>(2), c.arg<bool>(3)}, clock));
        });
    });
    golden::for_each(module, "render_export_document", [&](const golden::Case& c) {
        replay(c, [&] {
            std::vector<MeetingExport> entries;
            for (const auto& entry : c.args()[0]) {
                entries.push_back(entry_of(entry));
            }
            return Json(render_combined(entries, c.arg<std::string>(1),
                                        {c.arg<bool>(2), c.arg<bool>(3)}, clock));
        });
    });
}
