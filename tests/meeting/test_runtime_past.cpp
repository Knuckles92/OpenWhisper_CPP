// tests/test_meeting_runtime.py: Past Meetings (copy a transcript, delete, clear) and
// tests/test_automation_fix_pipeline_events.py::test_deleting_a_meeting_drops_the_replay_buffer.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "automation/event_bus.h"
#include "base/utf8.h"
#include "meeting/runtime_messages.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

bool any_contains(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::any_of(lines, [text](const std::string& line) {
        return lower(line).find(text) != std::string::npos;
    });
}

// A meeting whose spool folder exists under the meetings root.
std::filesystem::path meeting_with_spool(RuntimeRig& rig, const std::string& id) {
    const auto spool = rig.root.path() / "meetings" / id;
    std::filesystem::create_directories(spool);
    rig.make_meeting(id, Json{{"spool_dir", fv::path_to_utf8(spool)}});
    return spool;
}

} // namespace

// from tests/test_meeting_runtime.py::test_copy_past_meeting_transcript_exports_text
TEST_CASE("test_copy_past_meeting_transcript_exports_text", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_copy",
                     Json{{"title", "Planning"},
                          {"started_at", "2025-01-02T09:30:00"},
                          {"state_json", R"({"title": "Planning", "participants": {}})"}});
    fv::meeting_state::TranscriptSegment segment;
    segment.segment_id = "sg_1";
    segment.meeting_id = "m_copy";
    segment.channel = "mic";
    segment.text = "Hello team";
    rig.repo->add_segments({segment});

    const std::string text = rig.rt->copy_past_meeting_transcript("m_copy");

    CHECK_THAT(text, ContainsSubstring("Hello team"));
    CHECK_THAT(text, ContainsSubstring("Planning"));
}

// from tests/test_meeting_runtime.py::test_copy_past_meeting_transcript_empty
TEST_CASE("test_copy_past_meeting_transcript_empty", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_empty", Json{{"state_json", "{}"}});
    CHECK(rig.rt->copy_past_meeting_transcript("m_empty").empty());
    rig.drain();
    CHECK(any_contains(rig.got.statuses, "no transcript"));
}

// from tests/test_meeting_runtime.py::test_delete_past_meeting_refuses_while_live
TEST_CASE("test_delete_past_meeting_refuses_while_live", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.set_controller_active(true);
    probe.set_engine(std::make_shared<FakeEngine>("m_live"));
    rig.rt->delete_past_meeting("m_live");
    rig.drain();
    CHECK(any_contains(rig.got.statuses, "finish the current meeting"));
}

// from tests/test_meeting_runtime.py::test_delete_past_meeting_hides_card_and_deletes
TEST_CASE("test_delete_past_meeting_hides_card_and_deletes", "[test_meeting_runtime]") {
    RuntimeRig rig;
    const auto spool = meeting_with_spool(rig, "m_old");
    rig.probe().set_card("m_old");

    rig.probe().delete_past_meeting_worker("m_old");
    rig.drain();

    CHECK_FALSE(rig.repo->get_meeting("m_old").has_value());
    CHECK_FALSE(std::filesystem::exists(spool));
    CHECK_FALSE(rig.probe().card());
    CHECK(any_contains(rig.got.statuses, "deleted"));
}

// from tests/test_meeting_runtime.py::test_delete_past_meeting_can_keep_recordings
TEST_CASE("test_delete_past_meeting_can_keep_recordings", "[test_meeting_runtime]") {
    RuntimeRig rig;
    const auto spool = meeting_with_spool(rig, "m_keep");
    rig.rt->delete_past_meeting("m_keep", false);
    rig.drain();
    CHECK_FALSE(rig.repo->get_meeting("m_keep").has_value());
    CHECK(std::filesystem::exists(spool));
    CHECK(rig.got.refreshes == 1);
}

// from tests/test_meeting_runtime.py::test_clear_past_meetings_skips_live_id_and_hides_card
TEST_CASE("test_clear_past_meetings_skips_live_id_and_hides_card", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_live");
    rig.make_meeting("m_old");
    auto probe = rig.probe();
    probe.set_controller_active(true);
    probe.set_engine(std::make_shared<FakeEngine>("m_live"));
    probe.set_card("m_old");

    probe.clear_past_meetings_worker(false);
    rig.drain();

    CHECK(rig.repo->get_meeting("m_live").has_value());
    CHECK_FALSE(rig.repo->get_meeting("m_old").has_value());
    CHECK_FALSE(probe.card());
    CHECK(rig.got.statuses.back() == "Meetings cleared");
}

// from tests/test_meeting_runtime.py::test_clear_past_meetings_with_recordings_status
TEST_CASE("test_clear_past_meetings_with_recordings_status", "[test_meeting_runtime]") {
    RuntimeRig rig;
    const auto spool = meeting_with_spool(rig, "m_gone");
    rig.rt->clear_past_meetings(true);
    rig.drain();
    CHECK_FALSE(std::filesystem::exists(spool));
    CHECK(rig.got.refreshes == 1);
    CHECK(rig.got.statuses.back() == "Meetings and recordings cleared");
}

// from
// tests/test_automation_fix_pipeline_events.py::test_deleting_a_meeting_drops_the_replay_buffer
TEST_CASE("test_deleting_a_meeting_drops_the_replay_buffer",
          "[test_automation_fix_pipeline_events]") {
    // The process-wide bus with a replay buffer, as the replaying_bus fixture installs it.
    const auto bus = std::make_shared<fv::automation::EventBus>();
    const auto previous = fv::automation::set_bus(bus);
    bus->publish("meeting.segments",
                 Json{{"meeting_id", "m1"}, {"segments", Json::array({Json{{"text", "hi"}}})}});
    REQUIRE_FALSE(bus->recent().empty());
    {
        RuntimeRig rig;
        rig.make_meeting("m1");
        rig.rt->delete_past_meeting("m1", false);
        rig.drain();
        CHECK_FALSE(rig.repo->get_meeting("m1").has_value());
    }
    CHECK(bus->recent().empty());
    fv::automation::set_bus(previous);
    bus->close();
}

TEST_CASE("a meeting still re-running its steps cannot be deleted", "[meeting][runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_busy");
    FinalizationClaim claim("m_busy");
    rig.rt->delete_past_meeting("m_busy");
    rig.drain();
    CHECK(rig.got.statuses.back() == messages::kWaitPostMeeting);
    CHECK(rig.repo->get_meeting("m_busy").has_value());
    CHECK(rig.rt->meeting_busy("m_busy"));
    CHECK(rig.rt->meeting_in_use("m_busy"));
    claim.release();
    CHECK_FALSE(rig.rt->meeting_busy("m_busy"));
    CHECK_FALSE(rig.rt->meeting_in_use("m_busy"));
}
