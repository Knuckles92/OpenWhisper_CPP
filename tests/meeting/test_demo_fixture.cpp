// The developer-mode demo meeting (meeting/dev_fixture.py): the canned table and what seeding a
// store writes, replayed against OpenWhisper's vectors, and tests/test_meeting_dev_fixture.py.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "engine_support.h"
#include "golden/golden.h"
#include "meeting/demo_fixture.h"
#include "meeting_state/schema.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/store.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "meeting_state/testing/fixed_ids.h"

namespace golden = fv::golden;
using fv::Json;
using fv::meeting_state::MeetingState;
using fv::meeting_state::MeetingStateStore;

namespace {

// The case's recording repository: the stored segments, and segment_exists over them.
class SegmentsRepository {
public:
    std::shared_ptr<fv::meeting_state::testing::FakeMeetingRepository> inner =
        std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>();
};

std::optional<std::string> me_of(MeetingStateStore& store) {
    const auto results = store.apply("system", std::nullopt,
                                     Json::array({Json{{"op", "upsert_participant"},
                                                       {"display_name", "Me"},
                                                       {"kind", "me"},
                                                       {"is_provisional", false}}}));
    REQUIRE(results.at(0).ok);
    return results.at(0).target_id;
}

} // namespace

TEST_CASE("meeting.dev_fixture: the demo title, note, turns and speakers",
          "[golden][meeting.dev_fixture]") {
    golden::for_each("meeting.dev_fixture", "constants", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            Json{{"DEMO_TITLE", std::string(fv::meeting::DEMO_TITLE)},
                                 {"DEMO_NOTE", std::string(fv::meeting::DEMO_NOTE)}}));
    });
    golden::for_each("meeting.dev_fixture", "turns", [](const golden::Case& c) {
        Json turns = Json::array();
        for (const auto& turn : fv::meeting::demo_turns()) {
            turns.push_back(Json::array(
                {std::string(turn.speaker), turn.start_s, turn.end_s, std::string(turn.text)}));
        }
        CHECK(turns.size() == 25);
        CHECK(golden::equal(c.result<Json>(), turns));
    });
    golden::for_each("meeting.dev_fixture", "speakers", [](const golden::Case& c) {
        Json speakers = Json::array();
        for (const auto& speaker : fv::meeting::demo_speakers()) {
            speakers.push_back(Json::array(
                {std::string(speaker.key), std::string(speaker.name), std::string(speaker.kind)}));
        }
        CHECK(golden::equal(c.result<Json>(), speakers));
    });
}

TEST_CASE("meeting.dev_fixture: seeding a store writes OpenWhisper's state",
          "[golden][meeting.dev_fixture]") {
    golden::for_each("meeting.dev_fixture", "seed_demo_meeting", [](const golden::Case& c) {
        INFO(c.label());
        const Json title = c.arg<Json>(0);
        fv::FakeClock clock;
        fv::meeting_state::testing::FixedIds ids;
        SegmentsRepository repository;
        fv::meeting_state::SegmentPredicates predicates;
        predicates.segment_exists = [inner = repository.inner](std::string_view id) {
            return inner->segment_exists("m_demo", id);
        };
        MeetingState state;
        state.meeting_id = "m_demo";
        MeetingStateStore store(std::move(state), nullptr, {}, clock, ids, std::move(predicates));
        const std::optional<std::string> me = me_of(store);

        const auto result = fv::meeting::seed_demo_meeting(
            "m_demo", me, store, *repository.inner, nullptr,
            title.is_null() ? std::nullopt : std::optional<std::string>(title.get<std::string>()),
            ids);

        const Json expected = c.result<Json>();
        Json participant_ids = Json::object();
        for (const auto& key : {"me", "jordan", "sam"}) {
            const auto found = result.participant_ids.find(key);
            participant_ids[key] = found != result.participant_ids.end() && found->second
                                       ? Json(*found->second)
                                       : Json();
        }
        CHECK(golden::equal(expected.at("result"), Json{{"segment_count", result.segment_count},
                                                        {"last_end_s", result.last_end_s},
                                                        {"participant_ids", participant_ids}}));
        Json segments = Json::array();
        for (const Json& row : repository.inner->get_segments("m_demo", -1.0, std::nullopt)) {
            segments.push_back(Json::array(
                {row.at("id"), row.at("channel"), row.at("start_s"), row.at("end_s"),
                 row.at("text"), row.at("speaker_participant_id"), row.at("speaker_source")}));
        }
        CHECK(golden::equal(expected.at("segments"), segments));
        CHECK(golden::equal(expected.at("state"), store.snapshot()));
    });
}

TEST_CASE("test_seed_demo_meeting_writes_transcript_and_cards", "[test_meeting_dev_fixture]") {
    // from tests/test_meeting_dev_fixture.py::test_seed_demo_meeting_writes_transcript_and_cards
    fv::meeting::test::SqlRepository sql;
    const std::string meeting_id = fv::meeting_state::new_id("m");
    sql.repo->create_meeting(Json{{"id", meeting_id},
                                  {"title", ""},
                                  {"status", "active"},
                                  {"started_at", "2026-08-13T00:00:00"},
                                  {"ended_at", nullptr},
                                  {"paused_total_s", 0.0},
                                  {"host_token", "host"},
                                  {"guest_token", "guest"},
                                  {"cloud_enabled", true},
                                  {"asr_model", "base"},
                                  {"agent_provider", "openrouter"},
                                  {"agent_model", "test"},
                                  {"spool_dir", ""},
                                  {"state_json", nullptr},
                                  {"state_seq", 0},
                                  {"app_pid", 1},
                                  {"app_heartbeat_at", "2026-08-13T00:00:00"}});
    MeetingState state;
    state.meeting_id = meeting_id;
    state.cloud_enabled = true;
    fv::meeting_state::SegmentPredicates predicates;
    predicates.segment_exists = [repo = sql.repo, meeting_id](std::string_view id) {
        return repo->segment_exists(meeting_id, id);
    };
    MeetingStateStore store(std::move(state), sql.repo, {}, fv::system_clock(),
                            fv::meeting_state::random_ids(), std::move(predicates));
    const std::optional<std::string> me = me_of(store);

    const auto result = fv::meeting::seed_demo_meeting(meeting_id, me, store, *sql.repo);

    CHECK(result.segment_count >= 10);
    CHECK(result.last_end_s > 60);
    CHECK(sql.repo->get_segments(meeting_id, -1.0, std::nullopt).size() == result.segment_count);
    const Json snapshot = store.snapshot();
    CHECK(snapshot.at("title") == std::string(fv::meeting::DEMO_TITLE));
    CHECK_FALSE(snapshot.at("topic").at("current").get<std::string>().empty());
    CHECK_FALSE(snapshot.at("rolling_summary").get<std::string>().empty());
    CHECK_FALSE(snapshot.at("cards").at("key_points").empty());
    std::vector<Json> decisions;
    for (const Json& item : snapshot.at("cards").at("decisions")) {
        if (item.at("status") != "removed") {
            decisions.push_back(item);
        }
    }
    REQUIRE_FALSE(decisions.empty());
    CHECK(decisions[0].at("pinned") == true);
    CHECK(decisions[0].at("status") == "confirmed");
    CHECK_FALSE(snapshot.at("questions").empty());
}

TEST_CASE("the demo meeting re-anchors the meeting clock at its last turn",
          "[test_meeting_dev_fixture]") {
    // dev_fixture.py: clock.resume_from_recovery(last_end_s)
    fv::FakeClock clock;
    fv::meeting::MeetingClock meeting_clock(clock);
    auto repository = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(clock);
    MeetingState state;
    state.meeting_id = "m_demo";
    MeetingStateStore store(std::move(state), nullptr, {}, clock);
    const auto me = me_of(store);
    fv::meeting::seed_demo_meeting("m_demo", me, store, *repository, &meeting_clock);
    CHECK(meeting_clock.now_s() == 376.0);
}
