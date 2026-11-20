// Audit a24 (meeting engine): behaviours of meeting/engine.py the port implemented and no test
// pinned. Each case names the map row (docs/port/map/a24-meeting-engine.md) it pins.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "engine/error.h"
#include "engine_support.h"
#include "meeting_web/dashboard_engine.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::MeetingEngineProbe;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::FakeAsr;
using fv::meeting::test::mic_segment;

namespace {

using std::chrono::seconds;

EngineSetup transcript_only() {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = false;
    return setup;
}

// A provider whose meetings come online; End's passes are switched off where it is used.
class Online final : public fv::meeting::test::OnlineProvider {
public:
    Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return {{"status", "completed"}};
    }
    Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return {{"status", "completed"}};
    }
};

std::set<std::string> keys_of(const Json& object) {
    std::set<std::string> out;
    for (auto it = object.begin(); it != object.end(); ++it) {
        out.insert(it.key());
    }
    return out;
}

} // namespace

TEST_CASE("a24 audit: start writes OpenWhisper's meeting row, spool folder and Me",
          "[test_meeting_engine]") {
    // A24-01 (engine.py start): the row's columns, the spool folder named by the id's first
    // twelve characters, the "Me" participant and the transcript-only finalization.
    EngineRig rig;
    EngineSetup setup = transcript_only();
    setup.pid = 777;
    MeetingEngine& engine = rig.make(setup);
    const Json result = engine.start();
    const std::string id = engine.meeting_id();
    CHECK(keys_of(result) == std::set<std::string>{"meeting_id", "url", "host_url", "guest_url"});
    CHECK(result.at("meeting_id") == id);
    CHECK(id.starts_with("m_"));

    const Json row = rig.sql.repo->get_meeting(id).value();
    CHECK(row.at("status") == "active");
    CHECK(row.at("ended_at").is_null());
    CHECK(row.at("paused_total_s") == 0.0);
    CHECK(row.at("app_pid") == 777);
    CHECK(row.at("app_heartbeat_at") == row.at("started_at"));
    CHECK(row.at("title") == "Test meeting");
    // secrets.token_urlsafe(32): 43 characters, one pair per meeting.
    CHECK(row.at("host_token").get<std::string>().size() == 43);
    CHECK(row.at("guest_token").get<std::string>().size() == 43);
    CHECK(row.at("host_token") != row.at("guest_token"));
    const std::filesystem::path spool = rig.spool_root() / id.substr(0, 12);
    CHECK(fv::path_from_utf8(row.at("spool_dir").get<std::string>()) == spool);
    CHECK(std::filesystem::is_directory(spool));

    const Json state = engine.store()->snapshot();
    CHECK(state.at("status") == "active");
    CHECK(state.at("finalization").at("status") == "disabled");
    CHECK(state.at("finalization").at("message") == "AI insights are off for this meeting.");
    const auto me = MeetingEngineProbe(engine).me_participant_id();
    REQUIRE(me.has_value());
    bool found = false;
    for (const Json& participant : state.at("participants")) {
        if (participant.at("id") == *me) {
            found = true;
            CHECK(participant.at("display_name") == "Me");
            CHECK(participant.at("kind") == "me");
        }
    }
    CHECK(found);
}

TEST_CASE("a24 audit: an aborted start that captured audio keeps a failed meeting",
          "[test_meeting_engine]") {
    // A24-03 (engine.py _abort_start, _mark_aborted_failed): with a chunk saved the row stays,
    // marked failed, and the card says final insights never ran.
    EngineRig rig;
    EngineSetup setup = transcript_only();
    MeetingEngine* engine_ptr = nullptr;
    setup.server = [&rig, &engine_ptr](fv::meeting::test::FakeServer& server) {
        server.on_start = [&rig, &engine_ptr]() -> std::expected<std::string, std::string> {
            rig.register_chunk(engine_ptr->meeting_id(), 0, 0.0);
            return std::unexpected("port in use");
        };
    };
    MeetingEngine& engine = rig.make(setup);
    engine_ptr = &engine;
    CHECK_THROWS_WITH(engine.start(), "Meeting dashboard could not start: port in use");
    CHECK_FALSE(engine.is_active());
    const auto row = rig.sql.repo->get_meeting(engine.meeting_id());
    REQUIRE(row.has_value());
    CHECK(row->at("status") == "failed");
    CHECK_FALSE(row->at("ended_at").is_null());
    const Json finalization = engine.store()->snapshot().at("finalization");
    CHECK(finalization.at("status") == "unavailable");
    CHECK(finalization.at("message") == "Meeting failed to start before final insights could run.");
    CHECK(engine.store()->snapshot().at("status") == "failed");
}

TEST_CASE("a24 audit: a pause or resume that cannot be saved rolls the clock back",
          "[test_meeting_engine]") {
    // A24-04 (engine.py pause, resume): the status write failing restores the clock and raises.
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    engine.start();
    const std::string id = engine.meeting_id();
    engine.pause();
    REQUIRE(engine.clock().is_paused());
    rig.sql.repo->delete_meeting(id);
    CHECK_THROWS_WITH(engine.resume(), "Could not persist resumed meeting state");
    CHECK(engine.clock().is_paused());

    EngineRig other;
    MeetingEngine& second = other.make(transcript_only());
    second.start();
    other.sql.repo->delete_meeting(second.meeting_id());
    CHECK_THROWS_WITH(second.pause(), "Could not persist paused meeting state");
    CHECK_FALSE(second.clock().is_paused());
}

TEST_CASE("a24 audit: the status event and its dashboard copy carry the same fields",
          "[test_meeting_engine]") {
    // A24-07 (engine.py _emit_status): the listener payload may carry `note`, the WebSocket
    // message never does. FioriVoce adds cloud_enabled, which the desktop's AI insights switch
    // follows (insights turn on per meeting after approval).
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    MeetingEngineProbe(engine).emit_status();
    const std::set<std::string> fields{"status",
                                       "cloud_enabled",
                                       "intelligence_online",
                                       "diarization_available",
                                       "diarization_reason",
                                       "capture",
                                       "finalization",
                                       "live_highlights_status",
                                       "speech"};
    CHECK(keys_of(rig.events_of("status").back()) == fields);
    std::set<std::string> message_fields = fields;
    message_fields.insert("type");
    CHECK(keys_of(rig.server(0)->messages_of("status").back()) == message_fields);
}

TEST_CASE("a24 audit: an unavailable meeting model leaves the audio pending and says so",
          "[test_meeting_engine]") {
    // A24-09 (engine.py _report_unavailable_asr).
    EngineRig rig;
    EngineSetup setup = transcript_only();
    setup.asr = [&rig](const std::string& id, const std::string& model,
                       const std::string& language) {
        auto asr = std::make_shared<FakeAsr>(rig.sql.repo, id, model, language);
        asr->available = false;
        return asr;
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const auto errors = rig.events_of("error");
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.back() == Json{{"code", "asr_unavailable"},
                                {"message", "The transcription model is not available; audio is "
                                            "recorded and can be transcribed later."}});
    CHECK(probe.asr() == nullptr);
    const auto chunk = rig.register_chunk(engine.meeting_id(), 0, 0.0);
    probe.on_chunk(chunk);
    CHECK(rig.asr(0)->chunks().empty());
    CHECK(rig.sql.repo->get_pending_chunks(engine.meeting_id(), std::nullopt, {}).size() == 1);
}

TEST_CASE("a24 audit: client actions before start and note requests without AI insights",
          "[test_meeting_engine]") {
    // A24-51 (engine.py apply_client_action, undo, request_note_adjustment).
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    const Json op{{"op", "add_item"}, {"card", "key_points"}, {"text", "Early"}};
    const auto results = engine.apply_client_action("host", std::nullopt, op);
    REQUIRE(results.size() == 1);
    CHECK_FALSE(results[0].ok);
    CHECK(results[0].reason == "inactive");
    CHECK(results[0].op == op);
    CHECK(engine.undo(1, std::nullopt).empty());

    engine.start();
    CHECK_THROWS_MATCHES(
        engine.request_note_adjustment("Summarize", [](Json) {}), fv::meeting_web::EngineRefusal,
        Catch::Matchers::Message("Turn on AI insights in an active meeting first."));
    const auto applied = engine.apply_client_action("host", std::nullopt, op);
    REQUIRE(applied.size() == 1);
    CHECK(applied[0].ok);
}

TEST_CASE("a24 audit: the transcript and delivery cursors pass through to the repository",
          "[test_meeting_engine]") {
    // A24-55 (engine.py get_transcript, get_transcript_delivery_*).
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    CHECK(engine.get_transcript().empty());
    CHECK(engine.get_transcript_delivery_page().empty());
    CHECK(engine.get_transcript_delivery_cursor("cards") == 0);
    CHECK(engine.get_transcript_delivery_high_water() == 0);
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments(
        {mic_segment(id, "sg_a", 0.0, 1.0, "first"), mic_segment(id, "sg_b", 1.0, 2.0, "second")});
    const auto transcript = engine.get_transcript();
    REQUIRE(transcript.size() == 2);
    CHECK(transcript[0].at("id") == "sg_a");
    CHECK(engine.get_transcript(0.5).size() == 1);
    CHECK(engine.get_transcript(-1.0, 1).size() == 1);
    const auto page = engine.get_transcript_delivery_page(0, 300);
    REQUIRE(page.size() == 2);
    const std::int64_t high = engine.get_transcript_delivery_high_water();
    CHECK(high == page.back().at("delivery_seq").get<std::int64_t>());
    engine.advance_transcript_delivery_cursor("cards", high);
    CHECK(engine.get_transcript_delivery_cursor("cards") == high);
    CHECK(engine.get_transcript_delivery_cursor("notes") == 0);
    CHECK(engine.get_transcript_delivery_page(high, 300).empty());
}

TEST_CASE("a24 audit: End and shutdown do nothing for a meeting that never started",
          "[test_meeting_engine]") {
    // A24-58 (engine.py _begin_end, wait_for_end, shutdown).
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    engine.end();
    CHECK(engine.wait_for_end(seconds(1)));
    CHECK_FALSE(MeetingEngineProbe(engine).end_started());
    engine.shutdown();
    CHECK(rig.events_of("ended").empty());
    CHECK(rig.kinds().empty());
    CHECK(engine.meeting_id().empty());
}

TEST_CASE("a24 audit: End shows ending before capture stops and saves the paused time",
          "[test_meeting_engine]") {
    // A24-61 (engine.py _mark_ending): "ending" is saved and shown before the slow work; A24-62
    // (_persist_terminal_status): the row gets status, ended_at and paused_total_s.
    fv::FakeClock clock;
    EngineRig rig;
    EngineSetup setup = transcript_only();
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    std::string status_at_flush;
    std::string last_event_at_flush;
    std::size_t ended_at_flush = 0;
    probe.before_flush([&] {
        status_at_flush = engine.store()->snapshot().at("status").get<std::string>();
        last_event_at_flush = rig.events_of("status").back().at("status").get<std::string>();
        ended_at_flush = rig.events_of("ended").size();
    });
    engine.start();
    const std::string id = engine.meeting_id();
    engine.pause();
    clock.advance(seconds(5));
    engine.resume();
    clock.advance(seconds(3));
    EngineRig::end_and_wait(engine);
    CHECK(status_at_flush == "ending");
    CHECK(last_event_at_flush == "ending");
    CHECK(ended_at_flush == 0);
    const Json row = rig.sql.repo->get_meeting(id).value();
    CHECK(row.at("status") == "ended");
    CHECK_FALSE(row.at("ended_at").is_null());
    CHECK(row.at("paused_total_s") == 5.0);
}

TEST_CASE("a24 audit: a terminal status that cannot be saved fails the End",
          "[test_meeting_engine]") {
    // A24-62 (engine.py _persist_terminal_status): the store refusing the terminal status
    // raises, and the failed-End path still reports `ended`.
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    MeetingEngineProbe probe(engine);
    probe.before_flush([&] { rig.sql.repo->delete_meeting(engine.meeting_id()); });
    engine.start();
    EngineRig::end_and_wait(engine);
    const auto errors = rig.events_of("error");
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.back().at("code") == "end_failed");
    CHECK(errors.back().at("message") == "Could not persist terminal meeting state");
    const auto ended = rig.events_of("ended");
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].at("status") == "needs_recovery");
    CHECK(ended[0].at("error") == "Could not persist terminal meeting state");
}

TEST_CASE("a24 audit: End repairs the live draft before any final pass", "[test_meeting_engine]") {
    // A24-63 (engine.py _repair_live_draft): with AI insights and the re-decode off, the blank
    // topic is filled from the key point the host added during the meeting.
    EngineRig rig;
    EngineSetup setup = transcript_only();
    setup.options.end_redecode = false;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_1", 0.0, 2.0, "We ship on Friday.")});
    REQUIRE(engine
                .apply_client_action("host", std::nullopt,
                                     Json{{"op", "add_item"},
                                          {"card", "key_points"},
                                          {"text", "Ship on Friday"},
                                          {"evidence", Json::array({"sg_1"})}})
                .front()
                .ok);
    REQUIRE(engine.store()->snapshot().at("topic").at("current") == "");
    EngineRig::end_and_wait(engine);
    CHECK(engine.store()->snapshot().at("topic").at("current") == "Ship on Friday");
}

TEST_CASE("a24 audit: shutdown settles a finalization still pending or running",
          "[test_meeting_engine]") {
    // A24-35 (engine.py _interrupt_finalization_on_shutdown).
    struct Case {
        bool cloud;
        std::string before;
        std::string status;
        std::string message;
    };
    const std::vector<Case> cases{
        {true, "running", "failed", "Final insights were interrupted by application shutdown."},
        {true, "pending", "unavailable", "Final insights did not run before shutdown."},
        {false, "running", "disabled", "AI insights are off for this meeting."},
    };
    for (const Case& c : cases) {
        CAPTURE(c.cloud, c.before);
        EngineRig rig;
        EngineSetup setup{.options = EngineRig::default_options()};
        setup.options.cloud_enabled = c.cloud;
        setup.options.end_polish = false;
        setup.options.end_report = false;
        setup.intelligence = std::make_shared<Online>();
        MeetingEngine& engine = rig.make(setup);
        engine.start();
        EngineRig::end_and_wait(engine);
        REQUIRE(engine.store()->snapshot().at("cloud_enabled") == c.cloud);
        REQUIRE(engine.publish_finalization(Json{{"status", c.before}}));
        engine.shutdown();
        const Json finalization = engine.store()->snapshot().at("finalization");
        CHECK(finalization.at("status") == c.status);
        CHECK(finalization.at("message") == c.message);
        CHECK(rig.server(0)->stopped.load());
    }
}

TEST_CASE("a24 audit: only a human pin on a speaker reaches the diarizer",
          "[test_meeting_engine]") {
    // A24-70 (engine.py _apply_diarizer_pins): a refused op and the diarizer's own relabels
    // forward nothing; a host reassignment does.
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    REQUIRE(probe.diarizer() != nullptr);
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({fv::meeting::test::loopback_segment(id, "sg_p")});
    const std::string guest = engine.add_guest("Riley").at("id").get<std::string>();

    const auto refused = engine.apply_client_action("host", std::nullopt,
                                                    Json{{"op", "reassign_segment_speaker"},
                                                         {"segment_id", "sg_missing"},
                                                         {"participant_id", guest}});
    REQUIRE(refused.size() == 1);
    CHECK_FALSE(refused[0].ok);
    probe.on_diarizer_relabel({{"sg_p", guest}});
    REQUIRE(rig.sql.repo->get_segment(id, "sg_p")->at("speaker_participant_id") == guest);
    CHECK(rig.diarizer->pins().empty());

    const std::string me = probe.me_participant_id().value();
    REQUIRE(engine
                .apply_client_action("host", std::nullopt,
                                     Json{{"op", "reassign_segment_speaker"},
                                          {"segment_id", "sg_p"},
                                          {"participant_id", me}})
                .front()
                .ok);
    CHECK(rig.diarizer->pins() == std::vector<std::pair<std::string, std::string>>{{"sg_p", me}});
}

TEST_CASE("a24 audit: a terminal finalization that cannot be saved is shown, not claimed",
          "[test_meeting_engine]") {
    // A24-25 (engine.py _set_finalization, _emit_ephemeral_finalization): the card unlocks with
    // the outcome while the saved state keeps what it had.
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    rig.sql.repo->delete_meeting(engine.meeting_id());
    const std::size_t before = rig.events_of("status").size();
    CHECK_FALSE(engine.publish_finalization(
        Json{{"status", "completed"}, {"message", "Final insights are ready."}}));
    const auto statuses = rig.events_of("status");
    REQUIRE(statuses.size() == before + 1);
    CHECK(statuses.back().at("finalization").at("status") == "completed");
    CHECK(rig.server(0)->messages_of("status").back().at("finalization").at("status") ==
          "completed");
    CHECK(engine.store()->snapshot().at("finalization").at("status") == "disabled");
    // A running snapshot that cannot be saved shows nothing.
    CHECK_FALSE(engine.publish_finalization(Json{{"status", "running"}}));
    CHECK(rig.events_of("status").size() == before + 1);
}
