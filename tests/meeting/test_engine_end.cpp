// MeetingEngine End over in-process fakes (tests/test_meeting_engine.py): the event order, spool
// loss, failed Ends, the offline re-decode with the meeting's own diarizer, End racing start, the
// speaker step, the finalization of a transcript-only meeting, End's cleanup and report passes
// through an injected provider (run only while the live agent is online) and the live agent's
// stop, backup and note requests. test_engine_intelligence.cpp runs the same passes through the
// app's MeetingIntelligence.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <future>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/fake_clock.h"
#include "engine_support.h"
#include "llm/testing/fake_judge.h"
#include "meeting/testing/fake_intelligence.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_capture/session_audio.h"
#include "meeting_state/schema.h"
#include "meeting_state/testing/fixed_ids.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "speech/api_models.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::MeetingEngineProbe;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::Event;
using fv::meeting::test::FakeAsr;
using fv::meeting::test::mic_segment;

namespace {

using std::chrono::seconds;

EngineSetup setup_with(bool cloud_enabled) {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = cloud_enabled;
    return setup;
}

std::set<std::string> segment_ids(EngineRig& rig, const std::string& meeting_id) {
    std::set<std::string> ids;
    for (const Json& row : rig.sql.repo->get_segments(meeting_id, -1.0, std::nullopt)) {
        ids.insert(row.at("id").get<std::string>());
    }
    return ids;
}

Json step(const Json& finalization, std::string_view id) {
    for (const Json& entry : finalization.value("steps", Json::array())) {
        if (entry.value("id", std::string()) == id) {
            return entry;
        }
    }
    return Json();
}

std::size_t index_of(const std::vector<std::string>& kinds, std::string_view kind) {
    return static_cast<std::size_t>(std::find(kinds.begin(), kinds.end(), kind) - kinds.begin());
}

std::vector<std::int16_t> tone(double seconds_long, int rate = 16000) {
    std::vector<std::int16_t> out(static_cast<std::size_t>(seconds_long * rate));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::int16_t>(i % 64 < 32 ? 6000 : -6000);
    }
    return out;
}

} // namespace

TEST_CASE("test_spool_loss_is_visible_and_never_ends_as_complete", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_spool_loss_is_visible_and_never_ends_as_complete. Python sets a spool's health error;
    // here the microphone's first chunk cannot be written (a folder stands where its file goes),
    // which is a real spool failure of the same kind.
    fv::meeting_state::testing::FixedIds ids;
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.ids = &ids;
    // The first id is the meeting's: m_000000000001, whose spool folder is m_0000000000.
    std::filesystem::create_directories(rig.spool_root() / "m_0000000000" / "mic_00000.wav");
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    REQUIRE(engine.meeting_id() == "m_000000000001");
    const auto samples = tone(1.0);
    REQUIRE(rig.mics().at(0)->deliver(samples, 16000, engine.clock().monotonic()));

    engine.end();
    REQUIRE(engine.wait_for_end(seconds(10)));
    const Json capture = engine.store()->snapshot().at("capture");
    CHECK(capture.at("audio_incomplete") == true);
    CHECK(capture.at("integrity_error").get<std::string>().find("could not be written") !=
          std::string::npos);
    CHECK(fv::meeting_state::MeetingState::from_json(engine.store()->snapshot())
              .capture.at("audio_incomplete") == true);
    std::vector<std::string> codes;
    for (const Json& error : rig.events_of("error")) {
        codes.push_back(error.at("code").get<std::string>());
    }
    CHECK(std::find(codes.begin(), codes.end(), "capture_incomplete") != codes.end());
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "needs_recovery");
    const Json ended = rig.events_of("ended").back();
    CHECK(ended.at("capture_error").get<std::string>().find("could not be written") !=
          std::string::npos);
}

TEST_CASE("test_end_failure_still_emits_ended", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::test_end_failure_still_emits_ended
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    probe.before_flush([] { throw std::runtime_error("spool flush exploded"); });
    engine.start();
    const std::string id = engine.meeting_id();

    EngineRig::end_and_wait(engine);
    CHECK(probe.end_done());
    const auto errors = rig.events_of("error");
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.back().at("code") == "end_failed");
    CHECK(errors.back().at("message") == "spool flush exploded");
    const auto ended = rig.events_of("ended");
    REQUIRE_FALSE(ended.empty());
    CHECK(ended.back().at("meeting_id") == id);
    CHECK(ended.back().at("error") == "spool flush exploded");
    CHECK_FALSE(engine.is_active());
    CHECK(rig.sql.repo->get_meeting(id)->at("status") == "needs_recovery");
    const Json finalization = engine.store()->snapshot().at("finalization");
    CHECK(finalization.at("status") == "failed");
    CHECK(finalization.at("message") ==
          "Meeting end failed before final insights could finish: spool flush exploded");
    CHECK(rig.server(0)->messages_of("meeting_ended").size() == 1);
}

TEST_CASE("test_failure_after_terminal_write_keeps_the_meeting_ended", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_failure_after_terminal_write_keeps_the_meeting_ended
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    probe.final_highlights([] { throw std::runtime_error("highlight finalization exploded"); });
    engine.start();
    const std::string id = engine.meeting_id();

    EngineRig::end_and_wait(engine);
    CHECK(rig.events_of("error").back().at("code") == "end_failed");
    CHECK(rig.sql.repo->get_meeting(id)->at("status") == "ended");
    CHECK(engine.store()->snapshot().at("status") == "ended");
    CHECK(engine.store()->snapshot().at("finalization").at("status") == "failed");
    const auto ended = rig.events_of("ended");
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].at("status") == "ended");
}

TEST_CASE("test_summary_stats_use_the_clock_and_skip_removed_items", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_summary_stats_use_the_clock_and_skip_removed_items
    fv::FakeClock clock;
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const auto added = engine.store()->apply(
        "host", std::nullopt,
        Json::array({Json{{"op", "add_item"}, {"card", "key_points"}, {"text", "Keep this"}},
                     Json{{"op", "add_item"}, {"card", "key_points"}, {"text", "Drop that"}}}));
    const Json dropped = added.at(1).effect.at("item");
    engine.store()->apply("host", std::nullopt,
                          Json::array({Json{{"op", "remove_item"},
                                            {"id", dropped.at("id")},
                                            {"base_revision", dropped.at("revision")}}}));
    clock.advance(seconds(42));

    const Json stats = MeetingEngineProbe(engine).finalization_summary_stats();
    CHECK(stats.at("duration_s") == 42.0);
    CHECK(stats.at("key_points") == 1);
}

TEST_CASE("test_normal_end_emits_ended_and_marks_the_meeting", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_normal_end_emits_ended_and_marks_the_meeting (the scheduler's consolidation and the
    // agent core's shutdown are M8's)
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(true));
    engine.start();
    const std::string id = engine.meeting_id();

    EngineRig::end_and_wait(engine);

    const auto ended = rig.events_of("ended");
    REQUIRE(ended.size() == 1);
    CHECK(ended.back().at("canceled") == false);
    CHECK(ended.back().at("meeting_id") == id);
    CHECK(ended.back().at("status") == "ended");
    CHECK(ended.back().at("unfinished_chunks") == 0);
    CHECK(ended.back().at("capture_error").is_null());
    CHECK(rig.sql.repo->get_meeting(id)->at("status") == "ended");
    CHECK_FALSE(rig.sql.repo->get_meeting(id)->at("ended_at").is_null());
    // The dashboard heard meeting_ended before the final status (A24-57).
    const auto messages = rig.server(0)->messages();
    const auto ended_at = std::find_if(messages.begin(), messages.end(), [](const Json& message) {
        return message.value("type", std::string()) == "meeting_ended";
    });
    REQUIRE(ended_at != messages.end());
    CHECK(ended_at->at("status") == "ended");
    CHECK(std::any_of(ended_at, messages.end(), [](const Json& message) {
        return message.value("type", std::string()) == "status" &&
               message.value("status", std::string()) == "ended";
    }));
}

TEST_CASE("test_asr_released_follows_the_stop_and_trails_ended", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_asr_released_follows_the_stop_and_trails_ended. asr_released is the app's cue to
    // reload its own model: only once the meeting's ASR stopped, after `ended` (the offline pass
    // still decodes on that model).
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(true));
    engine.start();
    EngineRig::end_and_wait(engine);
    const auto kinds = rig.kinds();
    REQUIRE(index_of(kinds, "asr_released") < kinds.size());
    CHECK(index_of(kinds, "asr_released") > index_of(kinds, "ended"));
    CHECK(rig.asr(rig.asr_count() - 1)->stops.load() == 1);
    CHECK(rig.events_of("asr_released").back().at("meeting_id") == engine.meeting_id());
}

TEST_CASE("test_end_event_precedes_slow_consolidation", "[test_meeting_engine]") {
    // from
    // tests/test_meeting_engine.py::TestEndLifecycle::test_end_event_precedes_slow_consolidation
    class SlowConsolidation final : public fv::meeting::test::OnlineProvider {
    public:
        Event entered;
        Event release;

        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }

        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            entered.set();
            if (!release.wait(seconds(10))) {
                return {{"status", "failed"}, {"message", "The fixture release timed out."}};
            }
            return {{"status", "completed"}};
        }
    };
    EngineRig rig;
    auto intelligence = std::make_shared<SlowConsolidation>();
    auto setup = setup_with(true);
    setup.intelligence = intelligence;
    auto& engine = rig.make(std::move(setup));
    engine.start();
    engine.end();
    REQUIRE(intelligence->entered.wait());
    CHECK_FALSE(rig.events_of("ended").empty());
    CHECK_FALSE(engine.is_active());
    const auto held = rig.sql.repo->get_meeting(engine.meeting_id());
    CHECK(held.has_value());
    if (held) {
        CHECK(held->at("status") == "ended");
        CHECK(fv::py::loads(held->at("state_json").get<std::string>())
                  .at("finalization")
                  .at("status") == "running");
    }
    const auto statuses = rig.events_of("status");
    CHECK(std::ranges::any_of(statuses, [](const Json& event) {
        return event.contains("finalization") && event.at("finalization").at("status") == "running";
    }));
    intelligence->release.set();
    REQUIRE(engine.wait_for_end(seconds(10)));
    CHECK(engine.store()->snapshot().at("finalization").at("status") == "completed");
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
}

TEST_CASE("test_end_unhealthy_intelligence_finalization", "[test_meeting_engine]") {
    // from
    // tests/test_meeting_engine.py::TestEndLifecycle::test_end_unhealthy_intelligence_finalization
    class UnhealthyAtEnd final : public fv::meeting::IntelligenceProvider {
    public:
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        int final_passes = 0;

        bool available() const override { return true; }

        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return core;
        }

        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            ++final_passes;
            return {{"status", "completed"}};
        }

        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            ++final_passes;
            return {{"status", "completed"}};
        }
    };
    EngineRig rig;
    auto intelligence = std::make_shared<UnhealthyAtEnd>();
    auto setup = setup_with(true);
    setup.intelligence = intelligence;
    auto& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(engine.store()->snapshot().at("intelligence_online") == true);
    intelligence->core->set_healthy(false);

    EngineRig::end_and_wait(engine);

    CHECK(intelligence->final_passes == 0);
    CHECK(intelligence->core->shutdown_calls() == 1);
    CHECK(engine.store()->snapshot().at("finalization").at("status") == "unavailable");
    CHECK(engine.store()->snapshot().at("finalization").at("message") ==
          "Meeting intelligence is offline; final cloud insights could not run.");
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
}

TEST_CASE("End runs no final insights when meeting intelligence never came online",
          "[test_meeting_engine]") {
    // engine.py _end_worker and end_run.py EndRun.plan: with no live scheduler End plans no
    // cleanup or report, and the card reads _intelligence_offline_message, the sentence and the
    // reason the start failed.
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    intelligence->set_live(false);
    auto setup = setup_with(true);
    setup.intelligence = intelligence;
    auto& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(engine.store()->snapshot().at("intelligence_online") == false);

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(intelligence->polished().empty());
    CHECK(intelligence->consolidated().empty());
    CHECK(finalization.at("status") == "unavailable");
    CHECK(finalization.at("message") == "Meeting intelligence is offline; final cloud insights "
                                        "could not run. The meeting agent is not available.");
    CHECK(step(finalization, "polish").is_null());
    CHECK(step(finalization, "consolidation").is_null());
    // What listeners saw last says the same.
    const auto statuses = rig.events_of("status");
    REQUIRE_FALSE(statuses.empty());
    CHECK(statuses.back().at("finalization").at("status") == "unavailable");
    CHECK(statuses.back().at("finalization").at("message") == finalization.at("message"));
    REQUIRE_FALSE(rig.events_of("ended").empty());
    CHECK(rig.events_of("ended").back().at("status") == "ended");
    CHECK_FALSE(engine.agent_writes_allowed());
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
}

TEST_CASE("test_end_failed_consolidation_outcome", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::test_end_failed_consolidation_outcome
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    intelligence->script_consolidate(
        {{"status", "failed"}, {"message", "Final insights failed: fixture failure"}});
    auto setup = setup_with(true);
    setup.intelligence = intelligence;
    auto& engine = rig.make(std::move(setup));
    engine.start();

    EngineRig::end_and_wait(engine);

    CHECK(intelligence->consolidated() == std::vector<std::string>{engine.meeting_id()});
    const auto finalization = engine.store()->snapshot().at("finalization");
    CHECK(step(finalization, "consolidation").at("status") == "failed");
    CHECK(finalization.at("status") == "failed");
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
}

TEST_CASE("test_end_failed_polish_marks_step_failed", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::test_end_failed_polish_marks_step_failed
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    intelligence->script_polish(
        {{"status", "failed"}, {"message", "Transcript cleanup failed: fixture failure"}});
    auto setup = setup_with(true);
    setup.intelligence = intelligence;
    auto& engine = rig.make(std::move(setup));
    engine.start();

    EngineRig::end_and_wait(engine);

    CHECK(intelligence->polished() == std::vector<std::string>{engine.meeting_id()});
    CHECK(intelligence->consolidated() == std::vector<std::string>{engine.meeting_id()});
    const auto finalization = engine.store()->snapshot().at("finalization");
    CHECK(step(finalization, "polish").at("status") == "failed");
    CHECK(finalization.at("status") == "failed");
}

TEST_CASE("test_ended_precedes_slow_offline_pass", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::test_ended_precedes_slow_offline_pass
    Event entered;
    Event release;
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.end_redecode = true;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    rig.asr(0)->on_offline = [&entered, &release] {
        entered.set();
        release.wait();
        return std::vector<fv::meeting_state::TranscriptSegment>{};
    };
    engine.end();
    REQUIRE(entered.wait());
    CHECK_FALSE(rig.events_of("ended").empty());
    CHECK_FALSE(engine.is_active());
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
    release.set();
    REQUIRE(engine.wait_for_end(seconds(10)));
    CHECK(engine.store()->snapshot().at("finalization").at("status") == "disabled");
}

TEST_CASE("test_cloud_off_keeps_draft_when_offline_commit_disabled", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_cloud_off_keeps_draft_when_offline_commit_disabled
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_draft", 0.0, 1.0, "draft")});
    rig.asr(0)->set_offline_segments({mic_segment(id, "sg_clean", 0.0, 1.2, "clean")});
    const Json finalization = EngineRig::end_and_wait(engine);
    const auto ids = segment_ids(rig, id);
    CHECK(ids.contains("sg_draft"));
    CHECK_FALSE(ids.contains("sg_clean"));
    CHECK(rig.asr(0)->offline_passes.load() == 0);
    CHECK(finalization.at("status") == "disabled");
}

TEST_CASE("test_cloud_off_replaces_transcript_when_redecode_enabled", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_cloud_off_replaces_transcript_when_redecode_enabled
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.end_redecode = true;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_draft", 0.0, 1.0, "draft")});
    rig.asr(0)->set_offline_segments({mic_segment(id, "sg_clean", 0.0, 1.2, "clean")});
    const Json finalization = EngineRig::end_and_wait(engine);
    const auto ids = segment_ids(rig, id);
    CHECK(ids.contains("sg_clean"));
    CHECK_FALSE(ids.contains("sg_draft"));
    CHECK(finalization.at("status") == "disabled");
}

TEST_CASE("test_redecode_labels_speakers_with_the_meetings_own_diarizer", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_redecode_labels_speakers_with_the_meetings_own_diarizer. Python stubs
    // load_channel_session with four seconds of silence; here the system-audio session WAV in
    // the meeting's spool holds them.
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.end_redecode = true;
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    // The live diarizer lands before End, so the re-decode never builds a second one.
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const std::string id = engine.meeting_id();
    fv::meeting::test::write_wav(fv::meeting::session_wav_path(probe.spool_dir(), "loopback"), 4.0);
    rig.diarizer->set_next("p_guest");
    fv::meeting_state::TranscriptSegment from_them =
        mic_segment(id, "sg_sys", 1.0, 2.0, "from them");
    from_them.channel = "loopback";
    rig.asr(0)->set_offline_segments({mic_segment(id, "sg_mic", 0.0, 1.0, "from me"), from_them});

    EngineRig::end_and_wait(engine);

    std::map<std::string, Json> rows;
    for (const Json& row : rig.sql.repo->get_segments(id, -1.0, std::nullopt)) {
        rows[row.at("id").get<std::string>()] = row;
    }
    CHECK(rows.at("sg_mic").at("speaker_participant_id") == probe.me_participant_id().value());
    CHECK(rows.at("sg_sys").at("speaker_participant_id") == "p_guest");
    CHECK(rig.diarizer->assigned() == std::vector<std::string>{"sg_sys"});
    CHECK(rig.diarizers_made() == 1);
    Json last_redecode;
    for (const Json& payload : rig.events_of("status")) {
        const Json found = step(payload.value("finalization", Json::object()), "redecode");
        if (!found.is_null()) {
            last_redecode = found;
        }
    }
    CHECK(last_redecode == Json{{"id", "redecode"},
                                {"name", "Audio Re-transcription"},
                                {"status", "completed"},
                                {"detail", "High-accuracy re-decoding complete"}});
    const auto segment_events = rig.events_of("segments");
    REQUIRE_FALSE(segment_events.empty());
    std::set<std::string> items;
    for (const Json& row : segment_events.back().at("items")) {
        items.insert(row.at("id").get<std::string>());
    }
    CHECK(items == std::set<std::string>{"sg_mic", "sg_sys"});
    CHECK(segment_events.back().contains("removed_ids"));
}

TEST_CASE("test_end_racing_start_waits_for_the_pipeline", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_end_racing_start_waits_for_the_pipeline
    Event entered;
    Event release;
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.asr = [&](const std::string& id, const std::string& model, const std::string& language) {
        entered.set();
        release.wait(seconds(10));
        return std::make_shared<FakeAsr>(rig.sql.repo, id, model, language);
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    std::thread starter([&engine] { engine.start(); });
    REQUIRE(entered.wait());
    std::atomic<bool> end_returned{false};
    std::thread ender([&engine, &end_returned] {
        engine.end();
        end_returned = true;
    });
    // End must not run against the half-built pipeline.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK_FALSE(probe.end_started());
    CHECK_FALSE(end_returned.load());
    release.set();
    ender.join();
    starter.join();
    CHECK(probe.end_started());
    REQUIRE(engine.wait_for_end(seconds(10)));
    CHECK_FALSE(rig.events_of("ended").empty());
    // The pipeline start() built was torn down by the End, and the server outlives it.
    CHECK_FALSE(engine.is_active());
    CHECK_FALSE(rig.server(0)->stopped.load());
}

TEST_CASE("test_double_end_reuses_the_same_worker", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::test_double_end_reuses_the_same_worker
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    engine.start();
    engine.end();
    const auto first = probe.end_thread();
    engine.end();
    CHECK(probe.end_thread() == first);
    REQUIRE(engine.wait_for_end(seconds(10)));
    CHECK(rig.events_of("ended").size() == 1);
}

TEST_CASE("test_background_ready_waits_for_offline_pass", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::test_background_ready_waits_for_offline_pass
    Event entered;
    Event released;
    EngineRig rig;
    EngineSetup setup = setup_with(true);
    setup.options.end_redecode = true;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    std::atomic<int> stops_during_pass{-1};
    FakeAsr* asr = rig.asr(0).get();
    asr->on_offline = [&, asr] {
        entered.set();
        released.wait();
        stops_during_pass = asr->stops.load();
        return std::vector<fv::meeting_state::TranscriptSegment>{};
    };
    engine.end();
    REQUIRE(entered.wait());
    CHECK(rig.events_of("background_ready").empty());
    CHECK(rig.events_of("asr_released").empty());
    released.set();
    REQUIRE(engine.wait_for_end(seconds(10)));
    CHECK(stops_during_pass.load() == 0);
    const auto kinds = rig.kinds();
    REQUIRE(index_of(kinds, "background_ready") < kinds.size());
    CHECK(index_of(kinds, "background_ready") > index_of(kinds, "asr_released"));
    CHECK(index_of(kinds, "asr_released") > index_of(kinds, "ended"));
}

TEST_CASE("test_sparse_redecode_explains_preserved_transcript", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::test_sparse_redecode_explains_preserved_transcript.
    // Without meeting intelligence the final "disabled" state carries no steps (as OpenWhisper's
    // does with AI insights off), so the step is read from the last status that showed it.
    const bool guard = GENERATE(false, true);
    EngineRig rig;
    EngineSetup setup = setup_with(true);
    setup.options.end_redecode = true;
    setup.options.redecode_coverage_guard = guard;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_draft", 0.0, 2.0, "one two three four five")});
    rig.asr(0)->set_offline_segments({mic_segment(id, "sg_sparse", 0.0, 2.0, "one two")});
    const Json finalization = EngineRig::end_and_wait(engine);
    CHECK(finalization.at("status") == "disabled");
    Json redecode;
    for (const Json& payload : rig.events_of("status")) {
        const Json found = step(payload.value("finalization", Json::object()), "redecode");
        if (!found.is_null()) {
            redecode = found;
        }
    }
    REQUIRE_FALSE(redecode.is_null());
    CHECK(redecode.at("status") == (guard ? "failed" : "completed"));
    if (guard) {
        const std::string detail = redecode.at("detail").get<std::string>();
        CHECK(detail.find("2 words versus 5") != std::string::npos);
        CHECK(detail.find("80% coverage threshold") != std::string::npos);
    }
    std::vector<std::string> ids;
    for (const Json& row : rig.sql.repo->get_segments(id, -1.0, std::nullopt)) {
        ids.push_back(row.at("id").get<std::string>());
    }
    CHECK(ids == std::vector<std::string>{guard ? "sg_draft" : "sg_sparse"});
}

// ---- the speaker step ----

TEST_CASE("test_local_backend_omits_speaker_step", "[test_meeting_engine]") {
    // from
    // tests/test_meeting_engine.py::TestCloudSpeakerStep::test_local_backend_omits_speaker_step
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    const Json finalization = EngineRig::end_and_wait(engine);
    CHECK(step(finalization, "speaker_id").is_null());
    CHECK(finalization.at("status") == "disabled");
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
}

TEST_CASE("test_off_backend_omits_speaker_step_and_diarizer", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCloudSpeakerStep::
    // test_off_backend_omits_speaker_step_and_diarizer
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.speaker_id_backend = "off";
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    CHECK(MeetingEngineProbe(engine).diarizer() == nullptr);
    CHECK(rig.diarizers_made() == 0);
    CHECK(engine.store()->snapshot().at("diarization_available") == false);
    const Json finalization = EngineRig::end_and_wait(engine);
    CHECK(step(finalization, "speaker_id").is_null());
    CHECK(finalization.at("status") == "disabled");
}

TEST_CASE("test_openai_without_consent_skips_honestly", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCloudSpeakerStep::
    // test_openai_without_consent_skips_honestly
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.speaker_id_backend = "openai";
    setup.options.speaker_id_audio_consent = false;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const Json finalization = EngineRig::end_and_wait(engine);
    const Json speaker = step(finalization, "speaker_id");
    REQUIRE_FALSE(speaker.is_null());
    CHECK(speaker.at("status") == "completed");
    CHECK(fv::py::lower(speaker.at("detail").get<std::string>()).find("consent") !=
          std::string::npos);
}

TEST_CASE("test_openai_model_retired_early_is_skipped", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCloudSpeakerStep::
    // test_openai_model_retired_early_is_skipped. The cloud pass that learns of the retirement
    // from OpenAI is M7.21's; here the retirement is the gate's, past the shutdown date.
    fv::FakeClock clock;
    EngineRig rig;
    clock.set_utc(std::chrono::sys_days{std::chrono::year{2027} / 3 / 1});
    EngineSetup setup = setup_with(false);
    setup.clock = &clock;
    setup.options.speaker_id_backend = "openai";
    setup.options.speaker_id_audio_consent = true;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const Json result = MeetingEngineProbe(engine).run_cloud_speaker_pass({}, true);
    EngineRig::end_and_wait(engine);
    CHECK(result.at("ok") == false);
    CHECK(result.at("skipped") == true);
    CHECK(result.at("error") == std::string(fv::speech::kSpeakerModelRetiredMessage));
}

TEST_CASE("test_refused_gate_never_uploads_or_claims_to", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCloudSpeakerStep::
    // test_refused_gate_never_uploads_or_claims_to
    struct Case {
        const char* backend;
        bool consent;
        const char* reason;
    };
    const Case c =
        GENERATE(Case{"openai", false, "consent"}, Case{"local", true, "not set to OpenAI"});
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.speaker_id_backend = c.backend;
    setup.options.speaker_id_audio_consent = c.consent;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    std::vector<bool> announced;
    const Json result = MeetingEngineProbe(engine).run_cloud_speaker_pass(
        [&] { announced.push_back(true); }, false);
    CHECK(announced.empty());
    CHECK(result.at("skipped") == true);
    CHECK(result.at("error").get<std::string>().find(c.reason) != std::string::npos);
}

TEST_CASE("shutdown marks an interrupted finalization and stops the dashboard",
          "[test_meeting_engine]") {
    // engine.py shutdown and _interrupt_finalization_on_shutdown (A24-35): a live meeting ends
    // with the short drain and no finalization is left pending or running.
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    engine.shutdown();
    CHECK_FALSE(engine.is_active());
    CHECK(rig.events_of("ended").size() == 1);
    CHECK(rig.server(0)->stopped.load());
    const std::string status =
        engine.store()->snapshot().at("finalization").at("status").get<std::string>();
    CHECK(status != "pending");
    CHECK(status != "running");
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
}

TEST_CASE("publish_finalization persists and shows a snapshot computed elsewhere",
          "[test_meeting_engine]") {
    // engine.py publish_finalization
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    CHECK(engine.publish_finalization(Json{{"status", "running"},
                                           {"stage", "redecode"},
                                           {"current_step", 1},
                                           {"total_steps", 2},
                                           {"steps", Json::array({Json{{"id", "redecode"}}})}}));
    const Json finalization = engine.store()->snapshot().at("finalization");
    CHECK(finalization.at("status") == "running");
    CHECK(finalization.at("stage") == "redecode");
    CHECK(finalization.at("current_step") == 1);
    CHECK(rig.events_of("status").back().at("finalization").at("status") == "running");
}

TEST_CASE("End routes opt-in meetings to the injected final intelligence provider",
          "[test_meeting_engine]") {
    const bool enabled = GENERATE(false, true);
    EngineRig rig;
    EngineSetup setup = setup_with(enabled);
    auto provider = std::make_shared<fv::meeting::FakeIntelligence>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    const auto id = engine.meeting_id();

    EngineRig::end_and_wait(engine);

    const auto snapshot = engine.store()->snapshot();
    if (enabled) {
        CHECK(provider->polished() == std::vector<std::string>{id});
        CHECK(provider->consolidated() == std::vector<std::string>{id});
        CHECK(provider->last_context().store == engine.store());
        CHECK(step(snapshot.at("finalization"), "consolidation").at("status") == "completed");
    } else {
        CHECK(provider->polished().empty());
        CHECK(provider->consolidated().empty());
        CHECK(snapshot.at("finalization").at("status") == "disabled");
    }
    CHECK_FALSE(engine.agent_writes_allowed());
}

TEST_CASE("an opted-in meeting schedules committed transcript and stops the live core at End",
          "[test_meeting_engine]") {
    class LiveProvider final : public fv::meeting::IntelligenceProvider {
    public:
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        bool available() const override { return true; }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return core;
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
    };

    const bool optOut = GENERATE(false, true);
    fv::FakeClock clock;
    EngineRig rig;
    EngineSetup setup = setup_with(true);
    setup.clock = &clock;
    auto provider = std::make_shared<LiveProvider>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(engine.store()->snapshot().at("intelligence_online") == true);
    const auto id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_live", 0.0, 2.0, "One recorded decision")});
    clock.advance(seconds(135));
    MeetingEngineProbe(engine).notify_intelligence_segments(1);
    REQUIRE(provider->core->wait_for_passes(1, seconds(3)));
    const auto payload = provider->core->passes().front().payload;
    REQUIRE(payload.new_segments.size() == 1);
    CHECK(payload.new_segments.front().at("id") == "sg_live");

    provider->core->emit_activity({"thinking", "Reading transcript", "", "cards", ""});
    REQUIRE(engine.recent_agent_activity().size() == 1);
    if (optOut) {
        engine.set_cloud_enabled(false);
        CHECK_FALSE(engine.store()->snapshot().at("cloud_enabled").get<bool>());
        CHECK(engine.recent_agent_activity().empty());
        CHECK(provider->core->shutdown_calls() == 1);
    }
    EngineRig::end_and_wait(engine);
    CHECK(provider->core->cancel_calls() >= 1);
    CHECK(provider->core->shutdown_calls() == 1);
    CHECK(engine.store()->snapshot().at("intelligence_online") == false);
}

TEST_CASE("live intelligence can resume only with the approved exact destination",
          "[test_meeting_engine]") {
    class ToggleProvider final : public fv::meeting::IntelligenceProvider {
    public:
        const fv::meeting_agent::MeetingAgentRoute route{
            "pi", "fake", "scripted", Json{{"base_url", "http://127.0.0.1:10/v1"}}};
        bool approved = false;
        std::vector<std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore>> cores;
        bool available() const override { return true; }
        std::optional<fv::meeting_agent::MeetingAgentRoute> live_destination() const override {
            return route;
        }
        bool authorize_live_destination(
            const fv::meeting_agent::MeetingAgentRoute& requested) const override {
            return approved && requested == route;
        }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext& context,
                         const fv::CancelToken&) override {
            CHECK(context.approved_route == route);
            cores.push_back(std::make_shared<fv::meeting_agent::testing::FakeAgentCore>());
            return cores.back();
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
    };
    fv::FakeClock clock;
    EngineRig rig;
    auto setup = setup_with(false);
    setup.clock = &clock;
    auto provider = std::make_shared<ToggleProvider>();
    setup.intelligence = provider;
    auto& engine = rig.make(std::move(setup));
    engine.start();
    CHECK(engine.live_intelligence_destination() == provider->route);
    CHECK_THROWS(engine.set_cloud_enabled(true));
    CHECK_THROWS(engine.enable_live_intelligence(provider->route));
    CHECK(provider->cores.empty());
    auto changed = provider->route;
    changed.model = "other";
    provider->approved = true;
    CHECK_THROWS(engine.enable_live_intelligence(changed));
    fv::CancelSource expired;
    expired.cancel();
    CHECK_THROWS(engine.enable_live_intelligence(provider->route, std::nullopt, expired.token()));
    CHECK(provider->cores.empty());

    engine.enable_live_intelligence(provider->route);
    REQUIRE(provider->cores.size() == 1);
    CHECK(engine.store()->snapshot().at("cloud_enabled") == true);
    engine.set_cloud_enabled(false);
    CHECK(engine.store()->snapshot().at("cloud_enabled") == false);
    CHECK(provider->cores.front()->shutdown_calls() == 1);

    const auto id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_disabled", 0.0, 2.0, "Off interval")});
    provider->approved = false;
    CHECK_THROWS(engine.enable_live_intelligence(provider->route));
    provider->approved = true;
    engine.enable_live_intelligence(provider->route);
    REQUIRE(provider->cores.size() == 2);
    CHECK(engine.store()->snapshot().at("cloud_enabled") == true);
    CHECK(engine.store()->snapshot().at("intelligence_online") == true);
    rig.sql.repo->add_segments({mic_segment(id, "sg_resumed", 5.0, 8.0, "New transcript")});
    clock.advance(seconds(135));
    MeetingEngineProbe(engine).notify_intelligence_segments(1);
    REQUIRE(provider->cores.back()->wait_for_passes(1, seconds(3)));
    const auto delivered = provider->cores.back()->passes().front().payload.new_segments;
    REQUIRE(delivered.size() == 1);
    CHECK(delivered.front().at("id") == "sg_resumed");
    EngineRig::end_and_wait(engine);
    CHECK(provider->cores.back()->shutdown_calls() == 1);
    CHECK_THROWS(engine.enable_live_intelligence(provider->route));
}

TEST_CASE("an approved fallback agent takes over once when the primary cannot initialize",
          "[test_meeting_engine]") {
    class BackupProvider final : public fv::meeting::IntelligenceProvider {
    public:
        std::vector<fv::meeting_agent::MeetingAgentRoute> routes;
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> primary =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        bool failInitialize = false;
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        bool available() const override { return true; }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext& context,
                         const fv::CancelToken&) override {
            REQUIRE(context.approved_route);
            routes.push_back(*context.approved_route);
            if (routes.size() == 1) {
                if (failInitialize) {
                    throw std::runtime_error("The primary agent could not initialize.");
                }
                primary->set_healthy(false);
                return primary;
            }
            return core;
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
    };

    const bool allowBackup = GENERATE(false, true);
    const bool failInitialize = GENERATE(false, true);
    const fv::meeting_agent::MeetingAgentRoute primary{"pi", "fake", "scripted", Json()};
    const fv::meeting_agent::MeetingAgentRoute backup{"codex", "codex", "fake-backup", Json()};
    EngineRig rig;
    auto setup = setup_with(true);
    auto provider = std::make_shared<BackupProvider>();
    provider->failInitialize = failInitialize;
    setup.intelligence = provider;
    setup.options.llm_provider = primary.provider;
    setup.options.llm_model = primary.model;
    setup.options.agent_approval = fv::meeting::LiveIntelligenceApproval{
        primary, allowBackup ? std::optional(backup) : std::nullopt};
    auto& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(provider->routes.size() == (allowBackup ? 2u : 1u));
    CHECK(provider->primary->shutdown_calls() == (failInitialize ? 0 : 1));
    CHECK(provider->routes.front() == primary);
    CHECK(engine.store()->snapshot().at("intelligence_online") == allowBackup);
    const auto recorded = rig.sql.repo->get_meeting(engine.meeting_id());
    REQUIRE(recorded);
    CHECK(recorded->at("agent_provider") == (allowBackup ? backup.provider : primary.provider));
    if (allowBackup) {
        CHECK(provider->routes.back() == backup);
    }
    EngineRig::end_and_wait(engine);
    CHECK(provider->core->shutdown_calls() == (allowBackup ? 1 : 0));
}

TEST_CASE("repeated checkpoints hand undelivered transcript to an approved backup",
          "[test_meeting_engine]") {
    class BackupProvider final : public fv::meeting::IntelligenceProvider {
    public:
        using Core = fv::meeting_agent::testing::FakeAgentCore;
        std::shared_ptr<Core> primary = std::make_shared<Core>();
        std::shared_ptr<Core> backup = std::make_shared<Core>();
        std::atomic<int> created = 0;
        bool fail_backup = false;
        bool available() const override { return true; }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext& context,
                         const fv::CancelToken&) override {
            ++created;
            if (context.approved_route && context.approved_route->provider == "codex") {
                if (fail_backup) {
                    throw std::runtime_error("The backup executable is missing.");
                }
                return backup;
            }
            return primary;
        }
    };
    const auto outcome = GENERATE(0, 1, 2);
    const bool approved = outcome != 0;
    fv::FakeClock clock;
    EngineRig rig;
    auto setup = setup_with(true);
    setup.clock = &clock;
    auto provider = std::make_shared<BackupProvider>();
    provider->fail_backup = outcome == 2;
    const fv::meeting_agent::MeetingAgentRoute primary{"pi", "fake", "primary", Json()};
    const fv::meeting_agent::MeetingAgentRoute backup{"codex", "codex", "backup", Json()};
    setup.intelligence = provider;
    setup.options.agent_approval = fv::meeting::LiveIntelligenceApproval{
        primary, approved ? std::optional(backup) : std::nullopt};
    provider->primary->set_hook([](const auto&, auto*) {
        fv::meeting_agent::AgentResult failure;
        failure.error = "401: private provider detail";
        return failure;
    });
    auto& engine = rig.make(std::move(setup));
    engine.start();
    const auto id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_unsent", 0.0, 2.0, "Retain this decision")});
    const auto until = std::chrono::steady_clock::now() + seconds(3);
    while (std::chrono::steady_clock::now() < until) {
        clock.advance(seconds(140));
        MeetingEngineProbe(engine).notify_intelligence_segments(1);
        if (outcome == 1 && provider->backup->wait_for_passes(1, std::chrono::milliseconds(20)) &&
            engine.store()->snapshot().at("intelligence_online") == true) {
            break;
        }
        const auto finalization = engine.store()->snapshot().at("finalization");
        if ((!approved && finalization.at("status") == "unavailable") ||
            (outcome == 2 && finalization.at("message").get<std::string>().find(
                                 "The backup executable is missing.") != std::string::npos)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(provider->primary->passes().size() >= 3);
    CHECK(provider->created.load() == (approved ? 2 : 1));
    const auto finalization = engine.store()->snapshot().at("finalization");
    const auto message = finalization.at("message").get<std::string>();
    CHECK(message.find("rejected the API key (401)") != std::string::npos);
    CHECK(message.find("private provider detail") == std::string::npos);
    if (outcome == 1) {
        CHECK(finalization.at("status") == "pending");
        CHECK(message.find("backup agent took over") != std::string::npos);
        const auto passes = provider->backup->passes();
        REQUIRE_FALSE(passes.empty());
        REQUIRE_FALSE(passes.front().payload.new_segments.empty());
        CHECK(passes.front().payload.new_segments.front().at("id") == "sg_unsent");
        const auto saved = rig.sql.repo->get_meeting(id);
        REQUIRE(saved);
        CHECK(saved->at("agent_provider") == "codex");
    } else {
        CHECK(provider->backup->passes().empty());
        CHECK(finalization.at("status") == "unavailable");
        CHECK(engine.store()->snapshot().at("intelligence_online") == false);
        if (outcome == 2) {
            CHECK(message.find("The backup agent could not take over.") != std::string::npos);
            CHECK(message.find("The backup executable is missing.") != std::string::npos);
        }
    }
    EngineRig::end_and_wait(engine);
    CHECK(provider->primary->shutdown_calls() == 1);
    CHECK(provider->backup->shutdown_calls() == (outcome == 1 ? 1 : 0));
}

TEST_CASE("the optional local insight review starts after the meeting is saved",
          "[test_meeting_engine]") {
    EngineRig rig;
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    auto setup = setup_with(true);
    setup.options.insight_review = {
        {"enabled", true}, {"consent", "typesafe-text-v1"}, {"sensitivity", "normal"}};
    setup.intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    setup.settings = [] {
        return Json{{"typesafe_enabled", true},
                    {"meeting_insight_review", true},
                    {"meeting_insight_review_consent", "typesafe-text-v1"}};
    };
    setup.local_fast_judge = [judge] {
        return judge;
    };
    auto& engine = rig.make(std::move(setup));
    engine.start();
    const auto id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_review", 0.0, 2.0, "We may ship on Friday")});
    REQUIRE(engine.store()
                ->apply("agent", std::string("notes"),
                        Json::array({Json{{"op", "add_item"},
                                          {"card", "decisions"},
                                          {"text", "We will ship on Friday"},
                                          {"evidence", Json::array({"sg_review"})}}}))
                .front()
                .ok);
    std::atomic<bool> saved_first = false;
    judge->set_handler([&](const Json&, const Json&) -> std::optional<Json> {
        const auto saved = rig.sql.repo->get_meeting(id);
        saved_first = saved && saved->at("status") == "ended";
        return Json{{"support", {{"noul", 0.9}}},
                    {"acceptance", {{"noul", 0.1}}},
                    {"contradiction", {{"noul", 0.0}}},
                    {"asr_uncertain", {{"noul", 0.0}}}};
    });
    EngineRig::end_and_wait(engine);
    CHECK(saved_first.load());
    const auto review = engine.store()->snapshot().at("insight_review");
    CHECK(review.at("status") == "ready");
    REQUIRE(review.at("questions").size() == 1);
    CHECK(review.at("questions")[0].at("field") == "acceptance");
}

TEST_CASE("live note requests share the approved scheduler and refuse after opt-out",
          "[test_meeting_engine]") {
    class RequestProvider final : public fv::meeting::IntelligenceProvider {
    public:
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        bool available() const override { return true; }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return core;
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
    };
    EngineRig rig;
    auto setup = setup_with(true);
    auto provider = std::make_shared<RequestProvider>();
    setup.intelligence = provider;
    auto& engine = rig.make(std::move(setup));
    engine.start();
    provider->core->set_hook([](const auto&, auto*) {
        fv::meeting_agent::AgentResult answer;
        answer.ok = true;
        answer.op_results = {fv::meeting_state::OpResult{.ok = true},
                             fv::meeting_state::OpResult{.ok = false}};
        return answer;
    });
    std::promise<Json> requested;
    auto reply = requested.get_future();
    engine.request_note_adjustment("Summarize the second decision",
                                   [&](Json answer) { requested.set_value(std::move(answer)); });
    REQUIRE(reply.wait_for(seconds(3)) == std::future_status::ready);
    const auto answer = reply.get();
    CHECK(answer.at("ok") == true);
    CHECK(answer.at("applied") == 1);
    CHECK(answer.at("rejected") == 1);
    const auto passes = provider->core->passes();
    REQUIRE(passes.size() == 1);
    CHECK(passes[0].payload.state_snapshot.at("note_adjustment_request") ==
          "Summarize the second decision");
    engine.set_cloud_enabled(false);
    CHECK_THROWS(engine.request_note_adjustment("After revocation", [](Json) {}));
    EngineRig::end_and_wait(engine);
    CHECK(provider->core->passes().size() == 1);
}
