// Golden vectors of meeting.persist.repository, meeting.stored and meeting.content
// (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "meeting_state/content.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/stored.h"
#include "pyport/errors.h"
#include "sql_support.h"

namespace golden = fv::golden;
using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

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

// The cases pin now at FakeClock's default, 2026-01-01T00:00:00Z.
const fv::FakeClock& utc_clock() {
    static const fv::FakeClock clock;
    return clock;
}

std::optional<std::string> optional_text(const Json& value) {
    return value.is_string() ? std::optional<std::string>(value.get<std::string>()) : std::nullopt;
}

std::vector<detail::OldSegment> old_items(const Json& items) {
    std::vector<detail::OldSegment> out;
    for (const auto& item : items) {
        detail::OldSegment old;
        old.id = item.at("id").get<std::string>();
        old.start_s = item.at("start_s").get<double>();
        old.end_s = item.at("end_s").get<double>();
        old.pinned = item.at("pinned").get<bool>();
        old.speaker_participant_id = optional_text(item.at("speaker_participant_id"));
        old.speaker_source = item.at("speaker_source").get<std::string>();
        old.channel = item.at("channel").get<std::string>();
        out.push_back(std::move(old));
    }
    return out;
}

std::vector<TranscriptSegment> new_items(const Json& items) {
    std::vector<TranscriptSegment> out;
    for (const auto& item : items) {
        TranscriptSegment segment;
        segment.segment_id = item.at("segment_id").get<std::string>();
        segment.meeting_id = item.at("meeting_id").get<std::string>();
        segment.channel = item.at("channel").get<std::string>();
        segment.start_s = item.at("start_s").get<double>();
        segment.end_s = item.at("end_s").get<double>();
        segment.text = item.at("text").get<std::string>();
        segment.speaker_participant_id = optional_text(item.at("speaker_participant_id"));
        segment.speaker_source = item.at("speaker_source").get<std::string>();
        segment.speaker_pinned = item.at("speaker_pinned").get<bool>();
        out.push_back(std::move(segment));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> id_map_of(const Json& pairs) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& pair : pairs) {
        out.emplace_back(pair.at(0).get<std::string>(), pair.at(1).get<std::string>());
    }
    return out;
}

Json pairs_json(const std::vector<std::pair<std::string, std::string>>& pairs) {
    Json out = Json::array();
    for (const auto& [from, to] : pairs) {
        out.push_back(Json::array({from, to}));
    }
    return out;
}

// search_filter: the ids the Past Meetings query keeps for `needle` over these meetings.
Json search_ids(const std::string& needle, const Json& meetings) {
    SqlFixture f;
    for (const auto& meeting : meetings) {
        const std::string id = meeting.at("id").get<std::string>();
        f.repo->create_meeting(
            {{"id", id},
             {"title", meeting.value("title", "")},
             {"status", meeting.value("status", "ended")},
             {"started_at", meeting.value("started_at", "2026-01-01T09:00:00+00:00")},
             {"host_token", "h"},
             {"guest_token", "g"},
             {"spool_dir", "spool"},
             {"state_json", meeting.contains("state_json") ? meeting.at("state_json") : Json()}});
        std::vector<TranscriptSegment> segments;
        int index = 0;
        for (const auto& text : meeting.value("segments", Json::array())) {
            segments.push_back(make_segment(id, "sg_" + id + "_" + std::to_string(index), index,
                                            index + 1.0, text.get<std::string>()));
            ++index;
        }
        f.repo->add_segments(segments);
    }
    PastMeetingQuery query;
    query.limit = 501;
    query.include_running = true;
    query.query = needle;
    std::set<std::string> ids;
    for (const auto& row : f.repo->list_past_meeting_summaries(query)) {
        ids.insert(row.at("id").get<std::string>());
    }
    return Json(ids);
}

Json pill_json(const std::optional<std::pair<std::string, std::string>>& pill) {
    return pill ? Json::array({pill->first, pill->second}) : Json();
}

} // namespace

TEST_CASE("golden meeting.persist.repository", "[golden]") {
    const char* module = "meeting.persist.repository";
    golden::for_each(module, "interval_iou", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(interval_iou(c.arg<double>(0), c.arg<double>(1), c.arg<double>(2),
                                     c.arg<double>(3)));
        });
    });
    golden::for_each(module, "match_live_to_offline", [](const golden::Case& c) {
        replay(c, [&] {
            auto segments = new_items(c.args()[1]);
            const auto [id_map, used_old] =
                detail::match_live_to_offline(old_items(c.args()[0]), segments);
            Json out = Json::object();
            out["id_map"] = pairs_json(id_map);
            out["used_old"] = Json(used_old);
            out["new"] = Json::array();
            for (const auto& segment : segments) {
                out["new"].push_back(
                    {{"segment_id", segment.segment_id},
                     {"speaker_participant_id", segment.speaker_participant_id
                                                    ? Json(*segment.speaker_participant_id)
                                                    : Json()},
                     {"speaker_source", segment.speaker_source},
                     {"speaker_pinned", segment.speaker_pinned}});
            }
            return out;
        });
    });
    golden::for_each(module, "remap_state_evidence", [](const golden::Case& c) {
        replay(c, [&] {
            Json state = c.args()[0];
            std::set<std::string, std::less<>> retained;
            for (const auto& id : c.args()[4]) {
                retained.insert(id.get<std::string>());
            }
            detail::remap_state_evidence(state, id_map_of(c.args()[1]), old_items(c.args()[2]),
                                         new_items(c.args()[3]), retained);
            return state;
        });
    });
    golden::for_each(module, "content_summary", [](const golden::Case& c) {
        replay(c, [&] {
            return detail::content_summary(c.arg<std::int64_t>(0), c.arg<bool>(1),
                                           c.arg<std::int64_t>(2), c.arg<std::string>(3));
        });
    });
    golden::for_each(module, "search_filter", [](const golden::Case& c) {
        replay(c, [&] { return search_ids(c.arg<std::string>(0), c.args()[1]); });
    });
}

TEST_CASE("golden meeting.stored", "[golden]") {
    const char* module = "meeting.stored";
    const Json settings = Json::object();
    golden::for_each(module, "load_state", [&](const golden::Case& c) {
        replay(c, [&] {
            return load_state(c.args()[0], c.arg<bool>(1), settings, utc_clock()).to_json();
        });
    });
    golden::for_each(module, "load_state_with_id", [&](const golden::Case& c) {
        replay(c, [&] {
            return load_state(c.args()[0], c.arg<std::string>(1), false, settings, utc_clock())
                .to_json();
        });
    });
    golden::for_each(module, "compact_finalization_list_fields", [](const golden::Case& c) {
        replay(c, [&] { return compact_finalization_list_fields(c.args()[0]); });
    });
    golden::for_each(module, "finalization_from_meeting_row", [](const golden::Case& c) {
        replay(c, [&] { return finalization_from_meeting_row(c.args()[0]).to_json(); });
    });
    golden::for_each(module, "stored_state_dict", [](const golden::Case& c) {
        replay(c, [&] { return stored_state_dict(c.args()[0]); });
    });
    golden::for_each(module, "stored_endpoint", [](const golden::Case& c) {
        replay(c, [&] { return stored_endpoint(c.args()[0]).value_or(Json()); });
    });
}

TEST_CASE("golden meeting.content", "[golden]") {
    const char* module = "meeting.content";
    golden::for_each(module, "meeting_display_title", [](const golden::Case& c) {
        replay(c, [&] { return Json(meeting_display_title(c.args()[0])); });
    });
    golden::for_each(module, "fallback_meeting_title", [](const golden::Case& c) {
        replay(c, [&] { return Json(fallback_meeting_title(c.args()[0])); });
    });
    golden::for_each(module, "meeting_preview_text", [](const golden::Case& c) {
        replay(c, [&] { return Json(meeting_preview_text(c.args()[0])); });
    });
    golden::for_each(module, "meeting_insights_pill", [](const golden::Case& c) {
        replay(c, [&] { return pill_json(meeting_insights_pill(c.args()[0])); });
    });
    golden::for_each(module, "untitled_title_from_topic", [](const golden::Case& c) {
        replay(c, [&] { return Json(untitled_title_from_topic(c.args()[0])); });
    });
    golden::for_each(module, "build_untitled_title_ops", [](const golden::Case& c) {
        replay(c, [&] { return build_untitled_title_ops(c.args()[0]); });
    });
}
