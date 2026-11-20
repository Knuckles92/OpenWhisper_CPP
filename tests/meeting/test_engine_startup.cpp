// MeetingEngine start-up over in-process fakes (tests/test_meeting_engine.py): deferred
// processing, required services and system-audio policies, the meeting row and state the start
// writes, the store wiring and the demo meeting. Meeting intelligence's halves of these tests
// (agent cores, the scheduler, intelligence_online) are M8's.

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "engine/error.h"
#include "engine_support.h"
#include "meeting/demo_fixture.h"
#include "meeting_capture/sources.h"
#include "pyport/json.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::MeetingEngineProbe;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::Event;
using fv::meeting::test::FakeAsr;
using fv::meeting::test::FakeDiarizer;
using fv::meeting::test::SharedDiarizer;

namespace {

using std::chrono::seconds;

EngineSetup setup_with(bool cloud_enabled, bool processing_inline = true) {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = cloud_enabled;
    setup.processing_inline = processing_inline;
    return setup;
}

Json snapshot(MeetingEngine& engine) {
    return engine.store()->snapshot();
}

std::string start_error(MeetingEngine& engine) {
    try {
        engine.start();
    } catch (const std::exception& error) {
        return error.what();
    }
    FAIL("start() should have thrown");
    return {};
}

} // namespace

// ---- deferred processing ----

TEST_CASE("test_capture_dashboard_and_ordered_replay_do_not_wait_for_model",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_capture_dashboard_and_ordered_replay_do_not_wait_for_model
    Event entered;
    Event release;
    EngineRig rig;
    EngineSetup setup = setup_with(false, false);
    setup.asr = [&](const std::string& id, const std::string& model, const std::string& language) {
        auto asr = std::make_shared<FakeAsr>(rig.sql.repo, id, model, language);
        asr->on_load = [&entered, &release](const fv::CancelToken&) {
            entered.set();
            release.wait();
            return true;
        };
        return asr;
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    const Json result = engine.start();
    REQUIRE(entered.wait());
    CHECK_FALSE(result.at("url").get<std::string>().empty());
    CHECK(probe.capture().source("mic")->is_active());
    CHECK(probe.capture().source("loopback")->is_active());
    CHECK(rig.server_count() == 1);
    CHECK_FALSE(probe.asr_ready());
    const std::string id = engine.meeting_id();
    const auto later = rig.register_chunk(id, 1, 4.0);
    const auto earlier = rig.register_chunk(id, 0, 1.0);
    const auto loopback = rig.register_chunk(id, 0, 2.0, "loopback");
    for (const auto& chunk : {later, earlier, earlier, loopback}) {
        probe.on_chunk(chunk);
    }
    CHECK(rig.asr(0)->chunks().empty());
    CHECK(rig.sql.repo->get_pending_chunks(id, std::nullopt, {}).size() == 3);
    release.set();
    CHECK(engine.wait_for_processing(seconds(5)));
    CHECK(rig.asr(0)->chunk_ids() ==
          std::vector<std::int64_t>{earlier.chunk_id, loopback.chunk_id, later.chunk_id});
    // A registration can finish before its delayed spool notification.
    probe.on_chunk(later);
    const auto live = rig.register_chunk(id, 2, 6.0);
    probe.on_chunk(live);
    probe.on_chunk(live);
    CHECK(rig.asr(0)->chunk_ids() == std::vector<std::int64_t>{earlier.chunk_id, loopback.chunk_id,
                                                               later.chunk_id, live.chunk_id});
}

TEST_CASE("test_end_cancels_loading_and_leaves_pending_audio_recoverable",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_end_cancels_loading_and_leaves_pending_audio_recoverable (the agent-core and scheduler
    // assertions are M8's)
    Event entered;
    EngineRig rig;
    EngineSetup setup = setup_with(true, false);
    setup.asr = [&](const std::string& id, const std::string& model, const std::string& language) {
        auto asr = std::make_shared<FakeAsr>(rig.sql.repo, id, model, language);
        FakeAsr* raw = asr.get();
        asr->on_load = [&entered, raw](const fv::CancelToken& stop) {
            raw->available = false;
            entered.set();
            stop.wait_for(seconds(5));
            return false;
        };
        return asr;
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(entered.wait());
    const auto chunk = rig.register_chunk(engine.meeting_id(), 0, 0.0);
    probe.on_chunk(chunk);
    engine.end();
    REQUIRE(engine.wait_for_end(seconds(5)));
    CHECK(engine.wait_for_processing(seconds(5)));
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "needs_recovery");
    CHECK(rig.sql.repo->get_pending_chunks(engine.meeting_id(), std::nullopt, {}).at(0).at("id") ==
          chunk.chunk_id);
    CHECK(rig.asr(0)->chunks().empty());
    CHECK(rig.asr(0)->stops.load() >= 1);
    CHECK(probe.asr() == nullptr);
    CHECK(rig.events_of("asr_released").size() == 1);
}

TEST_CASE("test_failed_dashboard_preserves_audio_without_starting_models",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_failed_dashboard_preserves_audio_without_starting_models
    EngineRig rig;
    MeetingEngine* engine = nullptr;
    EngineSetup setup = setup_with(false, false);
    setup.server = [&](fv::meeting::test::FakeServer& server) {
        server.on_start = [&]() -> std::expected<std::string, std::string> {
            MeetingEngineProbe probe(*engine);
            probe.on_chunk(rig.register_chunk(engine->meeting_id(), 0, 0.0));
            return std::unexpected("dashboard failed");
        };
    };
    engine = &rig.make(setup);
    CHECK(start_error(*engine).find("dashboard failed") != std::string::npos);
    CHECK(rig.sql.repo->get_meeting(engine->meeting_id())->at("status") == "failed");
    CHECK(rig.sql.repo->get_pending_chunks(engine->meeting_id(), std::nullopt, {}).size() == 1);
    for (const auto& source : rig.mics()) {
        CHECK_FALSE(source->is_active());
    }
    for (const auto& source : rig.loopbacks()) {
        CHECK_FALSE(source->is_active());
    }
    CHECK_FALSE(MeetingEngineProbe(*engine).processing_thread_started());
    CHECK(rig.asr_count() == 0);
}

TEST_CASE("test_end_during_speaker_resolution_keeps_asr_and_never_adopts_late",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_end_during_speaker_resolution_keeps_asr_and_never_adopts_late (the agent-core and
    // scheduler assertions are M8's)
    Event entered;
    Event release;
    EngineRig rig;
    EngineSetup setup = setup_with(true, false);
    setup.options.diarization_model_resolver =
        [&entered, &release]() -> std::optional<std::filesystem::path> {
        entered.set();
        release.wait();
        return std::filesystem::path("/downloaded/speaker.onnx");
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    CHECK_FALSE(engine.start().at("url").get<std::string>().empty());
    REQUIRE(entered.wait());
    // ASR no longer queues behind the download.
    REQUIRE(fv::meeting::test::eventually([&] { return probe.asr_ready(); }));
    const auto chunk = rig.register_chunk(engine.meeting_id(), 0, 0.0);
    probe.on_chunk(chunk);
    CHECK(rig.asr(0)->chunk_ids() == std::vector<std::int64_t>{chunk.chunk_id});
    engine.end();
    REQUIRE(engine.wait_for_end(seconds(5)));
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "needs_recovery");
    CHECK(rig.sql.repo->get_pending_chunks(engine.meeting_id(), std::nullopt, {}).at(0).at("id") ==
          chunk.chunk_id);
    CHECK(rig.events_of("asr_released").size() == 1);
    // The resolver finished only after End claimed the meeting: its result never adopts a
    // diarizer or flips readiness on an ended meeting.
    release.set();
    CHECK(engine.wait_for_processing(seconds(5)));
    CHECK(probe.diarizer() == nullptr);
    const Json state = snapshot(engine);
    CHECK(state.at("diarization_available") == false);
    CHECK(state.at("diarization_reason") == "");
}

TEST_CASE("test_asr_and_intelligence_run_while_speaker_model_initializes",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_asr_and_intelligence_run_while_speaker_model_initializes (intelligence_online is
    // M8's)
    Event entered;
    Event release;
    EngineRig rig;
    EngineSetup setup = setup_with(true, false);
    setup.options.diarization_model_resolver =
        [&entered, &release]() -> std::optional<std::filesystem::path> {
        entered.set();
        release.wait();
        return std::filesystem::path("/downloaded/speaker.onnx");
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(entered.wait());
    REQUIRE(fv::meeting::test::eventually([&] { return probe.asr_ready(); }));
    // The readiness row shows the honest in-progress state meanwhile.
    Json state = snapshot(engine);
    CHECK(state.at("diarization_available") == false);
    CHECK(state.at("diarization_reason") == "");
    const auto statuses = rig.events_of("status");
    REQUIRE_FALSE(statuses.empty());
    CHECK(statuses.back().at("diarization_available") == false);
    // Chunks reach ASR while the speaker model still downloads.
    const auto chunk = rig.register_chunk(engine.meeting_id(), 0, 0.0);
    probe.on_chunk(chunk);
    CHECK(rig.asr(0)->chunk_ids() == std::vector<std::int64_t>{chunk.chunk_id});

    release.set();
    CHECK(engine.wait_for_processing(seconds(5)));
    CHECK(probe.diarizer() != nullptr);
    CHECK(rig.diarizers_made() == 1);
    state = snapshot(engine);
    CHECK(state.at("diarization_available") == true);
    CHECK(state.at("diarization_reason") == "");
    bool announced = false;
    for (const Json& payload : rig.events_of("status")) {
        announced = announced || payload.at("diarization_available") == true;
    }
    CHECK(announced);
}

TEST_CASE("test_end_during_diarizer_probe_prevents_late_availability", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_end_during_diarizer_probe_prevents_late_availability[availability] (the [callback]
    // case has no counterpart: relabels reach the engine through the diarizer host it is built
    // with, so there is no registration step to block in)
    Event entered;
    Event release;
    EngineRig rig;
    auto blocking = std::make_shared<FakeDiarizer>();
    blocking->on_probe = [&entered, &release] {
        entered.set();
        release.wait();
    };
    EngineSetup setup = setup_with(false, false);
    setup.diarizer = [blocking](const std::optional<std::filesystem::path>&) {
        return std::make_unique<SharedDiarizer>(blocking);
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(entered.wait());
    REQUIRE(fv::meeting::test::eventually([&] { return probe.asr_ready(); }));
    engine.end();
    REQUIRE(engine.wait_for_end(seconds(5)));
    CHECK(snapshot(engine).at("status") == "ended");
    release.set();
    CHECK(engine.wait_for_processing(seconds(5)));
    CHECK(probe.diarizer() == nullptr);
    CHECK(snapshot(engine).at("diarization_available") == false);
    const Json persisted = fv::py::loads(
        rig.sql.repo->get_meeting(engine.meeting_id())->at("state_json").get<std::string>());
    CHECK(persisted.at("diarization_available") == false);
}

TEST_CASE("test_committed_replay_frontier_survives_asr_startup", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_committed_replay_frontier_survives_asr_startup
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.asr = [&](const std::string& id, const std::string& model, const std::string& language) {
        auto asr = std::make_shared<FakeAsr>(rig.sql.repo, id, model, language);
        asr->after_requeue = [](FakeAsr& self) {
            const auto callback = self.segments_callback();
            for (const auto& chunk : self.chunks()) {
                callback(chunk, {});
            }
        };
        return asr;
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    probe.skip_processing();
    engine.start();
    const auto chunk = rig.register_chunk(engine.meeting_id(), 0, 2.0);
    probe.on_chunk(chunk);
    probe.start_asr(true);
    CHECK(probe.preview_frontiers() ==
          std::map<std::string, double, std::less<>>{{"mic", chunk.start_s + chunk.duration_s}});
    const std::size_t broadcasts = rig.server(0)->messages().size();
    probe.on_speech_preview(Json{{"channel", "mic"},
                                 {"text", "stale draft"},
                                 {"start_s", 0},
                                 {"end_s", 1},
                                 {"final", false}});
    CHECK(rig.server(0)->messages().size() == broadcasts);
}

TEST_CASE("test_engine_preview_feeds_listener_without_committing_transcript",
          "[test_meeting_voice_preview]") {
    // from tests/test_meeting_voice_preview.py::
    // test_engine_preview_feeds_listener_without_committing_transcript (the voice command
    // listener that observes the preview is M8's)
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    engine.start();
    probe.index_frontier("mic", 2.0);
    const Json payload{{"channel", "mic"},
                       {"start_s", 2},
                       {"end_s", 3},
                       {"text", "Assistant, note that."},
                       {"final", false}};
    probe.on_speech_preview(payload);
    auto previews = rig.server(0)->messages_of("speech_preview");
    REQUIRE(previews.size() == 1);
    CHECK(previews[0].at("text") == "Assistant, note that.");
    CHECK(previews[0].at("final") == false);
    engine.pause();
    Json later = payload;
    later["end_s"] = 4;
    probe.on_speech_preview(later);
    CHECK(rig.server(0)->messages_of("speech_preview").size() == 1);
}

// ---- required start-up services ----

TEST_CASE("test_server_failure_aborts_and_discards_empty_meeting", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestRequiredStartupServices::
    // test_server_failure_aborts_and_discards_empty_meeting
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.server = [](fv::meeting::test::FakeServer& server) {
        server.on_start = []() -> std::expected<std::string, std::string> {
            return std::unexpected(
                "Meeting Mode needs the dashboard dependencies (missing module: uvicorn).");
        };
    };
    MeetingEngine& engine = rig.make(setup);
    const std::string error = start_error(engine);
    CHECK(error.find("dashboard dependencies") != std::string::npos);
    CHECK(error.starts_with("Meeting dashboard could not start: "));
    CHECK_FALSE(engine.is_active());
    CHECK_FALSE(rig.sql.repo->get_meeting(engine.meeting_id()).has_value());
    CHECK(rig.events_of("server_started").empty());
}

TEST_CASE("test_zero_capture_sources_fail_before_dashboard_start", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestRequiredStartupServices::
    // test_zero_capture_sources_fail_before_dashboard_start
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.mics = 0;
    setup.loopback = false;
    MeetingEngine& engine = rig.make(setup);
    CHECK(start_error(engine).find("No audio devices") != std::string::npos);
    CHECK_FALSE(engine.is_active());
    CHECK_FALSE(rig.sql.repo->get_meeting(engine.meeting_id()).has_value());
    CHECK(rig.server_count() == 0);
}

TEST_CASE("test_missing_loopback_still_allows_mic_only_start", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestRequiredStartupServices::
    // test_missing_loopback_still_allows_mic_only_start
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.loopback = false;
    MeetingEngine& engine = rig.make(setup);
    const Json result = engine.start();
    CHECK_FALSE(result.at("host_url").get<std::string>().empty());
    CHECK(engine.is_active());
    MeetingEngineProbe probe(engine);
    CHECK(probe.capture().source("mic") != nullptr);
    CHECK(probe.capture().source("loopback") == nullptr);
}

TEST_CASE("test_required_system_audio_fails_when_loopback_missing", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestRequiredStartupServices::
    // test_required_system_audio_fails_when_loopback_missing
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.loopback = false;
    setup.options.system_audio_policy = "required";
    MeetingEngine& engine = rig.make(setup);
    CHECK(start_error(engine).find("LINUX_SYSTEM_AUDIO_REQUIRED") != std::string::npos);
    CHECK_FALSE(engine.is_active());
    CHECK_FALSE(rig.sql.repo->get_meeting(engine.meeting_id()).has_value());
}

TEST_CASE("test_disabled_system_audio_skips_loopback_and_watchdog", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestRequiredStartupServices::
    // test_disabled_system_audio_skips_loopback_and_watchdog
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.system_audio_policy = "disabled";
    MeetingEngine& engine = rig.make(setup);
    const Json result = engine.start();
    CHECK_FALSE(result.at("host_url").get<std::string>().empty());
    CHECK(engine.is_active());
    MeetingEngineProbe probe(engine);
    CHECK(probe.capture().source("mic") != nullptr);
    CHECK(probe.capture().source("loopback") == nullptr);
    const Json capture = snapshot(engine).at("capture");
    CHECK(capture.at("loopback_available") == false);
    CHECK(capture.at("message").get<std::string>().find("microphone-only choice") !=
          std::string::npos);
    // The watchdog does not reopen a disabled loopback.
    probe.capture().restart_channel("loopback");
    CHECK(probe.capture().source("loopback") == nullptr);
}

// ---- what the start writes ----

TEST_CASE("test_start_seeds_report_views", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::test_start_seeds_report_views
    EngineRig rig;
    MeetingEngine& first = rig.make(setup_with(true));
    first.start();
    CHECK(snapshot(first).at("report_views") == Json::array({"ribbon", "brief", "signal"}));

    EngineSetup setup = setup_with(true);
    setup.options.report_views = {"ribbon"};
    MeetingEngine& second = rig.make(setup);
    second.start();
    CHECK(snapshot(second).at("report_views") == Json::array({"ribbon"}));
}

TEST_CASE("test_start_persists_and_passes_llm_endpoint", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::
    // test_start_persists_and_passes_llm_endpoint (the meeting row's endpoint snapshot; the
    // agent core's configuration is M8's)
    EngineRig rig;
    const Json endpoint{{"profile_id", "custom_abcd1234"},
                        {"name", "LM Studio"},
                        {"kind", "custom"},
                        {"base_url", "http://127.0.0.1:1234/v1"},
                        {"api_key_env", ""}};
    EngineSetup setup = setup_with(true);
    setup.options.llm_provider = "custom_abcd1234";
    setup.options.llm_model = "local-qwen";
    setup.options.llm_endpoint = endpoint;
    MeetingEngine& engine = rig.make(setup);
    const Json result = engine.start();
    const auto meeting = rig.sql.repo->get_meeting(result.at("meeting_id").get<std::string>());
    REQUIRE(meeting);
    Json stored = meeting->at("agent_endpoint_json");
    if (stored.is_string()) {
        stored = fv::py::loads(stored.get<std::string>());
    }
    CHECK(stored.at("base_url") == endpoint.at("base_url"));
    CHECK(meeting->at("agent_provider") == "custom_abcd1234");
    CHECK(meeting->at("agent_model") == "local-qwen");
}

TEST_CASE("test_cloud_disabled_starts_no_intelligence", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::
    // test_cloud_disabled_starts_no_intelligence
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    CHECK(snapshot(engine).at("intelligence_online") == false);
    CHECK(rig.events_of("intelligence").empty());
}

TEST_CASE("test_asr_receives_pinned_meeting_language", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::
    // test_asr_receives_pinned_meeting_language
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.asr_language = "en";
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    REQUIRE(rig.asr(0));
    CHECK(rig.asr(0)->language == "en");
}

// ---- store wiring ----

TEST_CASE("test_pre_meeting_brief_is_live_before_the_first_word", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestStoreWiring::
    // test_pre_meeting_brief_is_live_before_the_first_word
    EngineRig rig;
    const std::string brief = "Flag anything about the Q3 budget, especially the numbers.";
    EngineSetup setup = setup_with(false);
    setup.options.intent = "  " + brief + "  ";
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const Json intent = snapshot(engine).at("intent");
    CHECK(intent.at("text") == brief);
    CHECK_FALSE(intent.at("updated_at").get<std::string>().empty());
}

TEST_CASE("test_no_brief_leaves_the_field_empty_and_unstamped", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestStoreWiring::
    // test_no_brief_leaves_the_field_empty_and_unstamped
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    CHECK(snapshot(engine).at("intent") ==
          Json{{"text", ""}, {"updated_at", ""}, {"author_id", ""}});
}

TEST_CASE("test_pinned_segments_are_protected_from_the_diarizer", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestStoreWiring::
    // test_pinned_segments_are_protected_from_the_diarizer
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({fv::meeting::test::loopback_segment(id, "sg_pinned")});
    const Json participant = engine.add_guest("Alex");
    const auto human = engine.apply_client_action("host", std::nullopt,
                                                  Json{{"op", "reassign_segment_speaker"},
                                                       {"segment_id", "sg_pinned"},
                                                       {"participant_id", participant.at("id")}});
    REQUIRE(human.at(0).ok);

    const Json other = engine.add_guest("Sam");
    const auto rejected =
        engine.store()->apply("system", std::string("diarizer"),
                              Json::array({Json{{"op", "reassign_segment_speaker"},
                                                {"segment_id", "sg_pinned"},
                                                {"participant_id", other.at("id")}}}));
    REQUIRE(rejected.size() == 1);
    CHECK_FALSE(rejected[0].ok);
    CHECK(rejected[0].reason == "segment_pinned");
    CHECK(rig.sql.repo->get_segment(id, "sg_pinned")->at("speaker_participant_id") ==
          participant.at("id"));
}

TEST_CASE("test_relabel_batch_skips_segments_not_yet_committed", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestStoreWiring::
    // test_relabel_batch_skips_segments_not_yet_committed
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({fv::meeting::test::loopback_segment(id, "sg_saved")});
    const std::string participant = engine.add_guest("Alex").at("id").get<std::string>();
    MeetingEngineProbe(engine).on_diarizer_relabel(
        {{"sg_saved", participant}, {"sg_in_flight", participant}});
    CHECK(rig.sql.repo->get_segment(id, "sg_saved")->at("speaker_participant_id") == participant);
}

// ---- the demo meeting ----

TEST_CASE("test_demo_mode_skips_live_audio_and_seeds_transcript", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDemoMeeting::
    // test_demo_mode_skips_live_audio_and_seeds_transcript (the scheduler is M8's)
    EngineRig rig;
    EngineSetup setup = setup_with(true);
    setup.options.demo_mode = true;
    MeetingEngine& engine = rig.make(setup);
    const Json result = engine.start();
    CHECK_FALSE(result.at("meeting_id").get<std::string>().empty());
    CHECK(engine.is_active());
    CHECK(engine.options().demo_mode);
    CHECK(rig.asr_count() == 0);
    MeetingEngineProbe probe(engine);
    CHECK(probe.capture().source("mic") == nullptr);
    CHECK(probe.capture().source("loopback") == nullptr);
    CHECK(probe.asr() == nullptr);
    CHECK(probe.diarizer() == nullptr);

    const auto segments = rig.sql.repo->get_segments(engine.meeting_id(), -1.0, std::nullopt);
    CHECK(segments.size() >= 10);
    bool june = false;
    for (const Json& row : segments) {
        june = june || row.value("text", std::string()).find("June") != std::string::npos;
    }
    CHECK(june);
    const Json state = snapshot(engine);
    CHECK_FALSE(state.at("title").get<std::string>().empty());
    CHECK_FALSE(state.at("topic").at("current").get<std::string>().empty());
    CHECK_FALSE(state.at("cards").at("key_points").empty());
    CHECK_FALSE(state.at("cards").at("decisions").empty());
    CHECK(state.at("capture").at("mic_available") == false);
    CHECK(state.at("capture").at("loopback_available") == false);
    CHECK(state.at("diarization_reason") == "The demo meeting uses channel labels.");
    // The note travels on the start's status, to the listeners only (A24-65).
    int noted = 0;
    for (const Json& payload : rig.events_of("status")) {
        noted += payload.value("note", std::string()) == fv::meeting::DEMO_NOTE ? 1 : 0;
    }
    CHECK(noted == 1);
    for (const Json& message : rig.server(0)->messages_of("status")) {
        CHECK_FALSE(message.contains("note"));
    }
}

// ---- finalization writes ----

TEST_CASE("test_terminal_finalization_adopts_topic_as_title", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::test_terminal_finalization_adopts_topic_as_title
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.title = "";
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    engine.store()->apply("system", std::string("seed"),
                          Json::array({Json{{"op", "set_topic"}, {"text", "Q3 roadmap"}}}));
    CHECK(snapshot(engine).at("title") == "");
    MeetingEngineProbe(engine).set_finalization(
        {.status = "completed", .message = "Final insights are ready.", .emit = false});
    CHECK(snapshot(engine).at("title") == "Q3 roadmap");
}

TEST_CASE("test_set_finalization_preserves_card_deferred", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::test_set_finalization_preserves_card_deferred
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    auto finalization = engine.store()->with_state(
        [](const fv::meeting_state::MeetingState& state) { return state.finalization; });
    finalization.card_deferred = true;
    REQUIRE(engine.store()->update_runtime_fields(Json{{"finalization", finalization.to_json()}}));
    MeetingEngineProbe(engine).set_finalization(
        {.status = "unavailable", .message = "Final insights are unavailable.", .emit = false});
    CHECK(engine.store()->with_state([](const fv::meeting_state::MeetingState& state) {
        return state.finalization.card_deferred;
    }));
}

TEST_CASE("test_capture_recovery_budget_fits_twelve_seconds", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::test_capture_recovery_budget_fits_twelve_seconds
    // Worst phase: one failed retry interval, one poll, and a loopback's first-block wait.
    const double worst = fv::meeting::CAPTURE_RETRY_INTERVAL_S +
                         fv::meeting::CAPTURE_WATCHDOG_INTERVAL_S +
                         fv::meeting::LOOPBACK_START_TIMEOUT_S;
    CHECK(worst <= fv::meeting::CAPTURE_RECOVERY_BUDGET_S);
    CHECK(fv::meeting::CAPTURE_RECOVERY_BUDGET_S <= 12.0);
}

// ---- guests, tokens and pause ----

TEST_CASE("guests are clipped and named, and rotated links reach the listeners",
          "[test_meeting_engine]") {
    // engine.py add_guest and regenerate_tokens (A24-50, A24-66)
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    CHECK_THROWS_AS(engine.add_guest("Early"), fv::EngineError);
    CHECK_THROWS_AS(engine.regenerate_tokens(), fv::EngineError);
    engine.start();
    CHECK(engine.add_guest("   ").at("display_name") == "Guest");
    CHECK(engine.add_guest(std::string(100, 'x')).at("display_name") == std::string(80, 'x'));
    const auto before = rig.sql.repo->get_meeting(engine.meeting_id());
    const Json links = engine.regenerate_tokens();
    CHECK(links == Json{{"host_url", "http://127.0.0.1:9999/h/token"},
                        {"guest_url", "http://127.0.0.1:9999/g/token"}});
    const auto after = rig.sql.repo->get_meeting(engine.meeting_id());
    CHECK(after->at("host_token") != before->at("host_token"));
    CHECK(after->at("guest_token") != before->at("guest_token"));
    CHECK(rig.server(0)->invalidations.load() == 1);
    CHECK(rig.events_of("server_started").back() ==
          Json{{"url", "http://127.0.0.1:9999"},
               {"host_url", "http://127.0.0.1:9999/h/token"},
               {"guest_url", "http://127.0.0.1:9999/g/token"}});
}

TEST_CASE("pause and resume freeze the clock and the status until End claims the meeting",
          "[test_meeting_engine]") {
    // engine.py pause and resume (A24-04)
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    engine.pause();
    CHECK(engine.clock().is_paused());
    CHECK(snapshot(engine).at("status") == "paused");
    CHECK(rig.events_of("status").back().at("status") == "paused");
    engine.resume();
    CHECK_FALSE(engine.clock().is_paused());
    CHECK(snapshot(engine).at("status") == "active");
    EngineRig::end_and_wait(engine);
    engine.pause();
    CHECK(snapshot(engine).at("status") == "ended");
}

TEST_CASE("a start after End or while running is refused", "[test_meeting_engine]") {
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    engine.start();
    CHECK(start_error(engine) == "Meeting engine is already active or finished");
    EngineRig::end_and_wait(engine);
    CHECK(start_error(engine) == "Meeting engine is already active or finished");
}

TEST_CASE("host-only messages never reach a transport without host-only delivery",
          "[test_meeting_engine]") {
    // engine.py _broadcast (A24-67): the transport's TypeError path.
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.server = [](fv::meeting::test::FakeServer& server) {
        server.host_only_support = false;
    };
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    // The speaker model's thread broadcasts its own status.
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const std::size_t before = rig.server(0)->messages().size();
    MeetingEngineProbe(engine).broadcast(Json{{"type", "agent_activity"}}, true);
    CHECK(rig.server(0)->messages().size() == before);
    MeetingEngineProbe(engine).broadcast(Json{{"type", "status"}}, false);
    CHECK(rig.server(0)->messages().size() == before + 1);
}
