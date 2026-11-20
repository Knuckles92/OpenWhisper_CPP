// MeetingEngine's capture and speaker labels over fake sources and a fake diarizer
// (tests/test_meeting_engine.py TestCaptureRecovery and TestDiarizationDegradation): the
// watchdog's restarts inside a running meeting, the session audio across a loopback restart, the
// diarization reasons, the degradation latch and pins after it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "base/testing/fake_clock.h"
#include "engine_support.h"
#include "meeting_capture/health.h"
#include "meeting_capture/session_audio.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::MeetingEngineProbe;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::eventually;
using fv::meeting::test::loopback_segment;

namespace {

using std::chrono::seconds;

EngineSetup setup_with(bool cloud_enabled) {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = cloud_enabled;
    return setup;
}

void advance(fv::FakeClock& clock, double seconds_ahead) {
    clock.advance(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(seconds_ahead)));
}

// TestDiarizationDegradation._prime: a registered system-audio chunk the loopback segments can
// be sliced out of (ten seconds of silence on disk).
fv::meeting_state::SpooledChunk prime(EngineRig& rig, MeetingEngine& engine) {
    const std::string id = engine.meeting_id();
    const std::int64_t seq = rig.sql.repo->next_chunk_seq(id, "loopback");
    const std::filesystem::path file =
        rig.sql.root.path() / ("loopback_" + std::to_string(seq) + ".wav");
    fv::meeting::test::write_wav(file, 10.0);
    const auto chunk = rig.register_chunk(id, seq, 0.0, "loopback", 10.0, file);
    MeetingEngineProbe(engine).index_chunk(chunk);
    return chunk;
}

} // namespace

// ---- capture recovery ----

TEST_CASE("test_open_portaudio_stream_without_callbacks_is_restarted", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCaptureRecovery::
    // test_open_portaudio_stream_without_callbacks_is_restarted. Python shortens the first-block
    // grace to 80 ms; here the clock moves past the real one.
    fv::FakeClock clock;
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.clock = &clock;
    setup.sources_need_blocks = true;
    setup.timing = fv::meeting::CaptureTiming{.watchdog_interval_s = 0.01, .retry_interval_s = 0.0};
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    const auto original = probe.capture().source("mic");
    advance(clock, fv::meeting::FIRST_BLOCK_GRACE_S + 0.5);
    CHECK(eventually([&] { return probe.capture().source("mic") != original; }, seconds(5)));
    CHECK(rig.mics().at(0)->stop_count() >= 1);
    CHECK(probe.capture().stall_channels().contains("mic"));
}

TEST_CASE("test_quiet_callbacks_keep_capture_live_without_signal_claim", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCaptureRecovery::
    // test_quiet_callbacks_keep_capture_live_without_signal_claim. Python shortens the stall
    // threshold to 80 ms and moves the monitor's clock 20 ms a block; here the clock moves one
    // second a block, past the first-block grace, while the real watchdog inspects the quiet
    // callbacks.
    fv::FakeClock clock;
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.clock = &clock;
    setup.sources_need_blocks = true;
    setup.timing = fv::meeting::CaptureTiming{.watchdog_interval_s = 0.01, .retry_interval_s = 3.0};
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    const auto mic = probe.capture().source("mic");
    auto* source = rig.mics().at(0).get();
    const std::vector<std::int16_t> quiet(1600, 0);
    for (int i = 0; i < 10; ++i) {
        source->deliver(quiet, 16000, engine.clock().monotonic());
        advance(clock, 1.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(probe.capture().source("mic") == mic);
    }
    probe.capture().update_status();
    const Json capture = engine.store()->snapshot().at("capture");
    CHECK(capture.at("mic_receiving") == true);
    CHECK(capture.at("mic_signal_windows") == 0);
}

TEST_CASE("test_microphone_stall_guidance_survives_microphone_only_message",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCaptureRecovery::
    // test_microphone_stall_guidance_survives_microphone_only_message. Python adds "mic" to the
    // stall set by hand; here the microphone really stalls (no block past the first-block grace).
    fv::FakeClock clock;
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.clock = &clock;
    setup.sources_need_blocks = true;
    setup.options.system_audio_policy = "disabled";
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    advance(clock, fv::meeting::FIRST_BLOCK_GRACE_S + 0.5);
    probe.capture().run_watchdog_pass();
    REQUIRE(probe.capture().stall_channels().contains("mic"));
    probe.capture().update_status();
    const std::string message =
        engine.store()->snapshot().at("capture").at("message").get<std::string>();
    CHECK(message.find("microphone-only choice") != std::string::npos);
    CHECK(message.find("no audio blocks are arriving") != std::string::npos);
}

TEST_CASE("test_default_loopback_change_restarts_only_that_channel", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCaptureRecovery::
    // test_default_loopback_change_restarts_only_that_channel
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.timing = fv::meeting::CaptureTiming{.watchdog_interval_s = 0.01, .retry_interval_s = 0.0};
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    const auto original_mic = probe.capture().source("mic");
    const auto original_loopback = probe.capture().source("loopback");
    const auto original_spool = probe.capture().spool("loopback");

    rig.audio_backends.at(0)->set_loopbacks(
        {{.id = "{a}", .name = "Speakers {a}", .is_default = false},
         {.id = "{b}", .name = "Speakers {b}", .is_default = true}});
    REQUIRE(eventually([&] {
        const auto candidate = probe.capture().source("loopback");
        return candidate != nullptr && candidate != original_loopback;
    }));
    const auto replacement = probe.capture().source("loopback");
    CHECK(std::get<std::string>(replacement->device_id()) == "{b}");
    CHECK(rig.loopbacks().at(0)->stop_count() >= 1);
    CHECK(probe.capture().source("mic") == original_mic);
    CHECK(original_mic->is_active());
    // The swap keeps the channel's spool writer.
    CHECK(probe.capture().spool("loopback") == original_spool);
    REQUIRE(eventually([&] {
        const Json capture = engine.store()->snapshot().at("capture");
        return capture.at("loopback_available") == true && capture.at("message") == "";
    }));
    const Json capture = engine.store()->snapshot().at("capture");
    CHECK(capture.at("mic_available") == true);
    CHECK(capture.at("loopback_available") == true);
    CHECK(capture.at("message") == "");
}

TEST_CASE("test_loopback_source_restart_preserves_spooled_session_audio", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestCaptureRecovery::
    // test_loopback_source_restart_preserves_spooled_session_audio: audio before and after the
    // restart both stay in the session WAV.
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    engine.start();
    const auto old_source = probe.capture().source("loopback");
    const auto old_spool = probe.capture().spool("loopback");
    REQUIRE(old_spool != nullptr);

    const std::vector<std::int16_t> first(3200, static_cast<std::int16_t>(0.25 * 32767));
    REQUIRE(rig.loopbacks().at(0)->deliver(first, 16000, engine.clock().monotonic()));
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    rig.audio_backends.at(0)->set_loopbacks(
        {{.id = "{b}", .name = "Speakers {b}", .is_default = true}});
    CHECK(probe.capture().restart_channel("loopback"));
    CHECK(probe.capture().spool("loopback") == old_spool);
    CHECK(probe.capture().source("loopback") != old_source);

    const std::vector<std::int16_t> second(3200, static_cast<std::int16_t>(0.5 * 32767));
    REQUIRE(rig.loopbacks().back()->deliver(second, 16000, engine.clock().monotonic()));
    if (const auto chunk = old_spool->flush()) {
        probe.on_chunk(*chunk);
    }

    const auto chunks = rig.sql.repo->get_audio_chunks(engine.meeting_id());
    const auto wav = fv::meeting::resolve_session_wav(probe.spool_dir(), "loopback", chunks);
    REQUIRE(wav);
    const auto [frames, rate, origin] =
        fv::meeting::load_channel_session(probe.spool_dir(), "loopback", chunks);
    CHECK(rate == fv::meeting::TARGET_RATE);
    REQUIRE(frames);
    // Pre- and post-restart audio both contributed samples (Python counts 3200 bytes).
    CHECK(frames->size() * 2 >= 3200);
}

// ---- diarization ----

TEST_CASE("test_failed_model_download_reason_reaches_the_dashboard", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDiarizationDegradation::
    // test_failed_model_download_reason_reaches_the_dashboard
    const std::string reason =
        "Could not reach Hugging Face to download the speaker model (network error).";
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.diarization_model_resolver = [reason]() -> std::optional<std::filesystem::path> {
        throw std::runtime_error(reason);
    };
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const Json state = engine.store()->snapshot();
    CHECK(state.at("diarization_available") == false);
    CHECK(state.at("diarization_reason") == reason);
    CHECK(MeetingEngineProbe(engine).diarizer() == nullptr);
    bool reached = false;
    for (const Json& message : rig.server(0)->messages_of("status")) {
        reached = reached || message.at("diarization_reason") == reason;
    }
    CHECK(reached);
}

TEST_CASE("test_diarization_reason_follows_the_outcome", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDiarizationDegradation::
    // test_diarization_reason_follows_the_outcome
    const auto [backend, expected] = GENERATE(
        std::pair<std::string, std::string>{"off", std::string(fv::meeting::DIARIZATION_OFF)},
        std::pair<std::string, std::string>{"local", ""});
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    setup.options.speaker_id_backend = backend;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    CHECK(engine.store()->snapshot().at("diarization_reason") == expected);
}

TEST_CASE("without a usable speaker model the reason names the cause", "[test_meeting_engine]") {
    // engine.py _start_diarizer: no model is "No speaker model is installed.", a model that does
    // not load is "The speaker model could not load on this computer.".
    const bool with_model = GENERATE(false, true);
    EngineRig rig;
    EngineSetup setup = setup_with(false);
    if (with_model) {
        setup.options.diarization_model_path = rig.sql.root.path() / "speaker.onnx";
    }
    setup.diarizer = [](const std::optional<std::filesystem::path>&) {
        return std::unique_ptr<fv::diarize::Diarizer>();
    };
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    CHECK(engine.store()->snapshot().at("diarization_reason") ==
          std::string(with_model ? fv::meeting::DIARIZATION_LOAD_FAILED
                                 : fv::meeting::DIARIZATION_NO_MODEL));
}

TEST_CASE("test_none_assignment_while_available_keeps_diarization_on", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDiarizationDegradation::
    // test_none_assignment_while_available_keeps_diarization_on
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const auto chunk = prime(rig, engine);
    rig.diarizer->set_next(std::nullopt); // a short segment: a normal "no label"
    probe.on_chunk_result(
        chunk, {loopback_segment(engine.meeting_id(), "sg_short", 1.0, 3.0, chunk.chunk_id)});
    CHECK(engine.store()->snapshot().at("diarization_available") == true);
    CHECK_FALSE(probe.degraded_diarization());
    CHECK_FALSE(rig.sql.repo->get_segments(engine.meeting_id(), -1.0, std::nullopt).empty());
}

TEST_CASE("test_degradation_flips_availability_once", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDiarizationDegradation::
    // test_degradation_flips_availability_once
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const auto chunk = prime(rig, engine);
    REQUIRE(engine.store()->snapshot().at("diarization_available") == true);
    const std::size_t initial_events = rig.events().size();
    const std::size_t initial_broadcasts = rig.server(0)->messages().size();

    rig.diarizer->available = false;
    rig.diarizer->set_next(std::nullopt);
    probe.on_chunk_result(
        chunk, {loopback_segment(engine.meeting_id(), "sg_1", 1.0, 3.0, chunk.chunk_id)});
    // A second chunk: chunk commits are idempotent.
    const auto second = prime(rig, engine);
    probe.on_chunk_result(
        second, {loopback_segment(engine.meeting_id(), "sg_2", 4.0, 6.0, second.chunk_id)});

    const Json state = engine.store()->snapshot();
    CHECK(state.at("diarization_available") == false);
    CHECK(state.at("diarization_reason") ==
          "Speaker separation stopped working during the meeting.");
    CHECK(probe.degraded_diarization());

    const auto events = rig.events();
    std::size_t notices = 0;
    for (std::size_t i = initial_events; i < events.size(); ++i) {
        if (events[i].first == "status" && events[i].second.at("diarization_available") == false) {
            ++notices;
        }
    }
    CHECK(notices == 1);
    const auto messages = rig.server(0)->messages();
    std::size_t broadcasts = 0;
    for (std::size_t i = initial_broadcasts; i < messages.size(); ++i) {
        if (messages[i].value("type", std::string()) == "status" &&
            messages[i].at("diarization_available") == false) {
            ++broadcasts;
        }
    }
    CHECK(broadcasts == 1);
    // Probed only until it latched: the start-up probe and the degradation's.
    CHECK(rig.diarizer->availability_probes.load() == 2);
    // The diarizer stays, so human corrections still reach pin().
    CHECK(probe.diarizer() != nullptr);
}

TEST_CASE("test_pin_still_works_after_degradation", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDiarizationDegradation::
    // test_pin_still_works_after_degradation
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const auto chunk = prime(rig, engine);
    rig.sql.repo->add_segments({loopback_segment(engine.meeting_id(), "sg_p")});
    rig.diarizer->available = false;
    probe.on_chunk_result(
        chunk, {loopback_segment(engine.meeting_id(), "sg_1", 1.0, 3.0, chunk.chunk_id)});
    REQUIRE(probe.degraded_diarization());

    const std::string participant = engine.add_guest("Riley").at("id").get<std::string>();
    const auto results = engine.apply_client_action("host", std::nullopt,
                                                    Json{{"op", "reassign_segment_speaker"},
                                                         {"segment_id", "sg_p"},
                                                         {"participant_id", participant}});
    REQUIRE(results.at(0).ok);
    CHECK(rig.diarizer->pins() ==
          std::vector<std::pair<std::string, std::string>>{{"sg_p", participant}});
}

TEST_CASE("live chunks label the microphone Me and system audio from the diarizer",
          "[test_meeting_engine]") {
    // engine.py _on_chunk_result and _assign_speakers (A24-10): Me on the microphone, the
    // diarizer's participant on system audio, the frontier broadcast and the segments event.
    EngineRig rig;
    MeetingEngine& engine = rig.make(setup_with(false));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const std::string guest = engine.add_guest("Jordan").at("id").get<std::string>();
    rig.diarizer->set_next(guest);
    const auto chunk = prime(rig, engine);
    fv::meeting_state::TranscriptSegment mine =
        fv::meeting::test::mic_segment(engine.meeting_id(), "sg_me", 0.5, 1.0, "mine");
    mine.chunk_id = chunk.chunk_id;
    probe.on_chunk_result(
        chunk, {mine, loopback_segment(engine.meeting_id(), "sg_them", 1.0, 3.0, chunk.chunk_id)});
    const auto me = rig.sql.repo->get_segment(engine.meeting_id(), "sg_me");
    const auto them = rig.sql.repo->get_segment(engine.meeting_id(), "sg_them");
    CHECK(me->at("speaker_participant_id") == probe.me_participant_id().value());
    CHECK(me->at("speaker_source") == "channel");
    CHECK(them->at("speaker_participant_id") == guest);
    CHECK(them->at("speaker_source") == "diarizer");
    const auto previews = rig.server(0)->messages_of("speech_preview");
    REQUIRE_FALSE(previews.empty());
    CHECK(previews.back() == Json{{"type", "speech_preview"},
                                  {"channel", "loopback"},
                                  {"text", ""},
                                  {"start_s", 0.0},
                                  {"end_s", 10.0},
                                  {"final", true}});
    REQUIRE(rig.events_of("segments").size() == 1);
    CHECK(rig.events_of("segments")[0].at("items").size() == 2);
    // A chunk already committed publishes nothing again.
    probe.on_chunk_result(chunk, {});
    CHECK(rig.events_of("segments").size() == 1);
}

// ---- microphone failover (tests/test_personalize_s7_audio_devices.py TestMeetingFailover) ----

namespace {

// OpenWhisper's two microphones: the laptop's (the default input, index 0 here) and a USB one
// the saved order ranks first (index 1 here; 1 and 2 there).
EngineSetup failover_setup() {
    EngineSetup setup = setup_with(false);
    setup.mic_devices = {
        {.name = "Laptop mic", .hostapi = 0, .max_input_channels = 1, .default_samplerate = 44100},
        {.name = "USB mic", .hostapi = 0, .max_input_channels = 1, .default_samplerate = 48000}};
    setup.options.mic_priority = {{"USB mic", "MME"}};
    return setup;
}

int device_index(const std::shared_ptr<fv::meeting::CaptureSource>& source) {
    return std::get<int>(source->device_id());
}

} // namespace

TEST_CASE("TestMeetingFailover::test_a_dead_microphone_is_excluded_before_reprobing",
          "[test_meeting_engine]") {
    // from tests/test_personalize_s7_audio_devices.py. OpenWhisper records find_mic_device's
    // (priority, exclude) arguments; here the real lookup runs over the fake backend, so the
    // priority shows in the USB mic chosen over the default input and the exclusion in the
    // reprobe landing on the laptop's.
    EngineRig rig;
    EngineSetup setup = failover_setup();
    setup.timing = fv::meeting::CaptureTiming{.watchdog_interval_s = 0.01, .retry_interval_s = 0.0};
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    const auto original = probe.capture().source("mic");
    REQUIRE(original != nullptr);
    CHECK(device_index(original) == 1);
    CHECK(probe.capture().mic_excluded().empty());

    rig.mics().at(0)->kill(); // unplugged: the stream went inactive
    REQUIRE(eventually(
        [&] {
            const auto current = probe.capture().source("mic");
            return current != nullptr && current != original;
        },
        seconds(2)));

    const auto replacement = probe.capture().source("mic");
    CHECK(device_index(replacement) == 0);
    CHECK(probe.capture().mic_excluded() == std::vector<fv::audio::DeviceKey>{{"USB mic", "MME"}});
}

TEST_CASE("TestMeetingFailover::test_a_microphone_that_will_not_open_falls_to_the_next",
          "[test_meeting_engine]") {
    // from tests/test_personalize_s7_audio_devices.py
    EngineRig rig;
    EngineSetup setup = failover_setup();
    setup.on_mic_source = [](fv::meeting::testing::FakeCaptureSource& source,
                             const fv::audio::InputDevice& device) {
        if (device.name == "USB mic") {
            source.fail_start("Device unavailable");
        }
    };
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();

    const auto mic = probe.capture().source("mic");
    REQUIRE(mic != nullptr);
    CHECK(device_index(mic) == 0);
    REQUIRE(rig.mics().size() == 2);
    CHECK(device_index(rig.mics()[0]) == 1);
}
