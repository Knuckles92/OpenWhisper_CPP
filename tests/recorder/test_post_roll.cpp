// Adaptive post-roll: capture after a stop press ends once the tail is quiet
// (tests/test_recorder_postroll.py, and the recorder case of tests/test_cancel_after_stop.py).
// Synthetic blocks reach the recorder through the fake device as sounddevice delivers them; the
// recorder's own threads still wait on the stop and quiet events exactly as in the app.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <optional>
#include <thread>
#include <vector>

#include "dsp/level.h"
#include "recorder/post_roll.h"
#include "recorder_test_support.h"

namespace {

using namespace fv::recorder_test;
using Catch::Matchers::WithinAbs;
using fv::PostRollGate;
using fv::Recorder;
using fv::RecorderOptions;
using Clock = std::chrono::steady_clock;

constexpr int kBlock = 1024;
constexpr int kRate = 44100;
constexpr double kBlockMs = kBlock * 1000.0 / kRate;
const int kQuietBlocks = static_cast<int>(std::ceil(300 / kBlockMs));

// Lead-in quiet, then speech with a short quiet gap every ~0.5 s.
std::vector<double> dictation(double seconds, double floor_db = -60.0, double speech_db = -25.0,
                              std::optional<double> gap = std::nullopt) {
    const double gap_db = gap.value_or(floor_db);
    const auto blocks = static_cast<int>(std::nearbyint(seconds * 1000 / kBlockMs));
    std::vector<double> levels;
    for (int i = 0; i < blocks; ++i) {
        if (i < 10 || i % 22 == 0 || i % 22 == 1 || i % 22 == 2) {
            levels.push_back(i >= 10 ? gap_db : floor_db);
        } else {
            levels.push_back(speech_db);
        }
    }
    return levels;
}

std::vector<double> repeat(double level, int count) {
    return std::vector<double>(static_cast<std::size_t>(count), level);
}

std::vector<double> concat(std::initializer_list<std::vector<double>> parts) {
    std::vector<double> all;
    for (const auto& part : parts) {
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

fv::audio::testing::FakeStream& device(Rig& rig) {
    return *rig.backend.opened().back().fake;
}

void feed(Rig& rig, const std::vector<double>& levels, std::uint64_t seed = 0) {
    for (std::size_t i = 0; i < levels.size(); ++i) {
        device(rig).deliver(noise_block(levels[i], seed + i));
    }
    REQUIRE(rig.rec().sync_capture());
}

// Feeds post-stop blocks; how many went in before the gate fired, or nullopt.
std::optional<int> feed_until_end(Rig& rig, const std::vector<double>& levels,
                                  std::uint64_t seed = 10'000) {
    for (std::size_t i = 0; i < levels.size(); ++i) {
        device(rig).deliver(noise_block(levels[i], seed + i));
        REQUIRE(rig.rec().sync_capture());
        if (!rig.rec().snapshot().post_roll_end_reason.empty()) {
            return static_cast<int>(i + 1);
        }
    }
    return std::nullopt;
}

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

} // namespace

// from tests/test_recorder_postroll.py::test_speech_then_silence_ends_after_the_quiet_window
TEST_CASE("speech then silence ends after the quiet window", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    LogCapture logs;
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(4.0));
    const auto started = Clock::now();
    REQUIRE(rig.rec().stop_recording());

    // The last word runs 150 ms past the press, then the room goes quiet.
    const auto ended = feed_until_end(rig, concat({repeat(-25.0, 6), repeat(-60.0, 60)}));

    CHECK(ended == 6 + kQuietBlocks);
    CHECK(rig.rec().snapshot().post_roll_end_reason == "quiet");
    REQUIRE(rig.rec().wait_for_stop_completion());
    CHECK(seconds_since(started) < 1.2 / 2);
    CHECK_FALSE(rig.rec().is_recording());
    // abort(), not stop(): the buffered remainder is post-roll anyway.
    const auto stream = rig.backend.opened().back();
    CHECK(stream.fake->aborted());
    CHECK(stream.fake->closed());
    const auto lines = logs.starting_with("Post-roll ended");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].starts_with("Post-roll ended by quiet after "));
    CHECK(lines[0].find("quiet below -48 dBFS") != std::string::npos);
    // Only what was captured before the gate fired is saved.
    CHECK(rig.rec().snapshot().recorded_frames ==
          static_cast<std::int64_t>(dictation(4.0).size() + static_cast<std::size_t>(*ended)) *
              kBlock);
}

// from tests/test_recorder_postroll.py::test_pause_shorter_than_the_window_does_not_end_capture
TEST_CASE("pause shorter than the window does not end capture", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(3.0));
    REQUIRE(rig.rec().stop_recording());

    // "uh ... what": a pause one block short of the window, then more speech.
    const auto tail =
        concat({repeat(-60.0, kQuietBlocks - 1), repeat(-28.0, 8), repeat(-60.0, 40)});
    const auto ended = feed_until_end(rig, tail);

    CHECK(ended == static_cast<int>(tail.size()) - 40 + kQuietBlocks);
}

// from tests/test_recorder_postroll.py::test_continued_speech_runs_to_the_cap
TEST_CASE("continued speech runs to the cap", "[test_recorder_postroll]") {
    Rig rig;
    RecorderOptions options;
    options.timings.post_roll = std::chrono::milliseconds(150);
    rig.make(options);
    LogCapture logs;
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(3.0));
    const auto started = Clock::now();
    REQUIRE(rig.rec().stop_recording());
    CHECK_FALSE(feed_until_end(rig, repeat(-25.0, 80)).has_value());
    REQUIRE(rig.rec().wait_for_stop_completion(std::chrono::milliseconds(2000)));
    CHECK(seconds_since(started) >= 0.14);
    CHECK(logs.starting_with("Post-roll ended by cap after ").size() == 1);
}

// from tests/test_recorder_postroll.py::test_noise_only_recording_runs_to_the_cap
TEST_CASE("noise only recording runs to the cap", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, repeat(-55.0, 150)); // nobody spoke: floor and "speech" coincide
    REQUIRE(rig.rec().stop_recording());

    CHECK_FALSE(feed_until_end(rig, repeat(-70.0, 60)).has_value());
    const PostRollGate gate = rig.rec().snapshot().post_roll_gate;
    CHECK_FALSE(gate.threshold_db().has_value());
    CHECK(gate.fallback().find("above the noise floor") != std::string::npos);
}

// from tests/test_recorder_postroll.py::test_loud_room_falls_back_to_the_cap
TEST_CASE("loud room falls back to the cap", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    // Speech only 10 dB over the room: a 12 dB margin cannot separate them.
    feed(rig, dictation(4.0, -35.0, -25.0));
    REQUIRE(rig.rec().stop_recording());

    CHECK_FALSE(feed_until_end(rig, repeat(-60.0, 60)).has_value());
    CHECK(rig.rec().snapshot().post_roll_gate.fallback() ==
          "speech only 10 dB above the noise floor");
}

// from tests/test_recorder_postroll.py::test_disabled_switch_keeps_the_fixed_post_roll
TEST_CASE("disabled switch keeps the fixed post roll", "[test_recorder_postroll]") {
    Rig rig;
    RecorderOptions options;
    options.post_roll.adaptive = false;
    rig.make(options);
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(4.0));
    REQUIRE(rig.rec().stop_recording());
    CHECK_FALSE(feed_until_end(rig, repeat(-60.0, 60)).has_value());

    const auto shot = rig.rec().snapshot();
    CHECK(shot.post_roll_gate.fallback() == "adaptive post-roll disabled");
    REQUIRE(shot.post_roll_cap.has_value());
    CHECK(*shot.post_roll_cap == std::chrono::milliseconds(1200));
    CHECK(fv::RecorderTimings{}.post_roll == std::chrono::milliseconds(1200));
}

// from tests/test_recorder_postroll.py::test_very_short_recording_runs_to_the_cap
TEST_CASE("very short recording runs to the cap", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    const int shortest = static_cast<int>(500 / kBlockMs) - 1;
    feed(rig, concat({repeat(-60.0, 4), repeat(-25.0, shortest - 4)}));
    REQUIRE(rig.rec().stop_recording());

    CHECK_FALSE(feed_until_end(rig, repeat(-60.0, 60)).has_value());
    CHECK(rig.rec().snapshot().post_roll_gate.fallback().find("too short for a noise floor") !=
          std::string::npos);
}

// from tests/test_recorder_postroll.py::test_short_recording_with_a_lead_in_uses_the_gate
TEST_CASE("short recording with a lead in uses the gate", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    const int blocks = static_cast<int>(std::ceil(500 / kBlockMs)) + 2;
    feed(rig, concat({repeat(-60.0, 6), repeat(-25.0, blocks - 6)}));
    REQUIRE(rig.rec().stop_recording());

    CHECK(feed_until_end(rig, repeat(-60.0, 60)) == kQuietBlocks);
}

// from tests/test_recorder_postroll.py::test_stop_before_any_audio_runs_to_the_cap
TEST_CASE("stop before any audio runs to the cap", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    REQUIRE(rig.rec().stop_recording());
    CHECK_FALSE(feed_until_end(rig, repeat(-90.0, 60)).has_value());
}

// from tests/test_recorder_postroll.py::test_near_digital_silence_floor_still_ends_early
TEST_CASE("near digital silence floor still ends early", "[test_recorder_postroll]") {
    // Noise suppression gates pauses to about -95 dBFS. Faint rustle at -70 dBFS is 25 dB over
    // that floor but 45 dB under the speech.
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(5.0, -95.0, -25.0));
    REQUIRE(rig.rec().stop_recording());

    const PostRollGate gate = rig.rec().snapshot().post_roll_gate;
    REQUIRE(gate.floor_db().has_value());
    CHECK(*gate.floor_db() <= -94.0);
    CHECK_THAT(*gate.threshold_db(), WithinAbs(*gate.speech_db() - 30.0, 1e-9));
    std::vector<double> tail;
    for (int i = 0; i < 30; ++i) {
        tail.push_back(-70.0);
        tail.push_back(-95.0);
    }
    CHECK(feed_until_end(rig, tail) == kQuietBlocks);
}

// from tests/test_recorder_postroll.py::test_near_digital_silence_floor_keeps_soft_speech
TEST_CASE("near digital silence floor keeps soft speech", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(5.0, -95.0, -25.0));
    REQUIRE(rig.rec().stop_recording());

    // A trailing word 15 dB softer than the dictation is still speech.
    const auto tail = concat({repeat(-95.0, 4), repeat(-40.0, 10), repeat(-95.0, 40)});
    CHECK(feed_until_end(rig, tail) == 14 + kQuietBlocks);
}

// from tests/test_recorder_postroll.py::test_repeat_stop_keeps_the_first_cap
TEST_CASE("repeat stop keeps the first cap", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(2.0));
    REQUIRE(rig.rec().stop_recording());
    const auto cap = rig.rec().snapshot().post_roll_cap;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(rig.rec().stop_recording()); // e.g. cancel pressed during post-roll
    CHECK(rig.rec().snapshot().post_roll_cap == cap);
}

// from tests/test_recorder_postroll.py::test_cleanup_does_not_wait_out_the_post_roll
TEST_CASE("cleanup does not wait out the post roll", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(2.0));
    // Journal tests cover disk durability; time only the post-roll wakeup here.
    REQUIRE(rig.rec().journal()->finish());
    LogCapture logs;
    const auto started = Clock::now();
    rig.rec().cleanup();
    CHECK(seconds_since(started) < 0.5);
    CHECK_FALSE(rig.rec().is_recording());
    CHECK(rig.rec().wait_for_stop_completion(std::chrono::milliseconds(0)));
    CHECK(logs.starting_with("Post-roll ended by cleanup after ").size() == 1);
}

// from tests/test_recorder_postroll.py::test_next_recording_gets_a_fresh_gate
TEST_CASE("next recording gets a fresh gate", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, dictation(2.0));
    REQUIRE(rig.rec().stop_recording());
    feed_until_end(rig, repeat(-60.0, 60));
    REQUIRE(rig.rec().wait_for_stop_completion());

    REQUIRE(rig.rec().start_recording());
    const auto shot = rig.rec().snapshot();
    CHECK_FALSE(shot.post_roll_gate.armed());
    CHECK(shot.post_roll_gate.frames_before_stop() == 0);
    CHECK(shot.post_roll_end_reason.empty());
    CHECK_FALSE(shot.stop_requested);
}

// from tests/test_recorder_postroll.py::test_block_level_db
TEST_CASE("block level db", "[test_recorder_postroll]") {
    std::vector<std::int16_t> sine(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        // np.sin(...) * 32767 cast to int16 truncates toward zero.
        sine[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(
            std::sin(2 * 3.141592653589793 * 1000 * i / double(kRate)) * 32767);
    }
    CHECK_THAT(fv::dsp::block_level_db(sine), WithinAbs(-3.0, 0.1));
    CHECK(fv::dsp::block_level_db(std::vector<std::int16_t>(kBlock, 0)) == -120.0);
    CHECK_THAT(fv::dsp::block_level_db(std::vector<float>(kBlock, 0.1f)), WithinAbs(-20.0, 0.01));
    CHECK(fv::dsp::block_level_db(std::vector<std::int16_t>{}) == -120.0);
}

// from tests/test_recorder_postroll.py::test_gate_percentiles_come_from_the_whole_recording
TEST_CASE("gate percentiles come from the whole recording", "[test_recorder_postroll]") {
    PostRollGate gate(kRate);
    for (const double level : concat({repeat(-60.0, 30), repeat(-20.0, 70)})) {
        gate.observe(level, kBlock);
    }
    gate.arm();
    CHECK(gate.floor_db() == -60.0);
    CHECK(gate.speech_db() == -20.0);
    CHECK(gate.threshold_db() == -48.0);
    // Blocks after the stop no longer move the floor.
    gate.observe(-90.0, kBlock);
    CHECK(gate.frames_after_stop() == kBlock);
    CHECK(gate.floor_db() == -60.0);
}

// --- a paused walkthrough --------------------------------------------------------------------

// from
// tests/test_recorder_postroll.py::test_nothing_is_kept_while_paused_so_later_moments_stay_in_step
TEST_CASE("nothing is kept while paused so later moments stay in step",
          "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, repeat(-25.0, 10));
    REQUIRE(rig.rec().pause());
    CHECK(rig.rec().paused());

    feed(rig, repeat(-25.0, 30), 100);
    CHECK(rig.rec().snapshot().recorded_frames == 10 * kBlock);
    CHECK_THAT(rig.rec().get_recording_duration(), WithinAbs(10.0 * kBlock / kRate, 1e-12));

    rig.rec().resume();
    feed(rig, repeat(-25.0, 5), 200);
    CHECK(rig.rec().snapshot().recorded_frames == 15 * kBlock);
}

// from
// tests/test_recorder_postroll.py::test_blocks_still_arriving_while_paused_keep_the_microphone_alive
TEST_CASE("blocks still arriving while paused keep the microphone alive",
          "[test_recorder_postroll]") {
    // Python reads _last_callback_at; here a short stall limit would move the recording to the
    // next microphone if the paused blocks did not count as the microphone being alive.
    Rig rig;
    RecorderOptions options;
    options.timings.stall_after_blocks = std::chrono::milliseconds(300);
    rig.make(options);
    REQUIRE(rig.rec().start_recording());
    REQUIRE(rig.rec().pause());
    for (int i = 0; i < 12; ++i) {
        device(rig).deliver(noise_block(-60.0, static_cast<std::uint64_t>(i)));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(rig.backend.opened().size() == 1);
    CHECK(rig.rec().snapshot().device_switches == 0);
    CHECK(rig.rec().snapshot().recorded_frames == 0);
}

// from tests/test_recorder_postroll.py::test_done_while_paused_stops_without_waiting_for_a_tail
TEST_CASE("done while paused stops without waiting for a tail", "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    feed(rig, repeat(-25.0, 10));
    REQUIRE(rig.rec().pause());
    const auto started = Clock::now();

    REQUIRE(rig.rec().stop_recording());
    REQUIRE(rig.rec().wait_for_stop_completion());

    CHECK(rig.rec().snapshot().post_roll_end_reason == "paused");
    CHECK(seconds_since(started) < 1.2 / 2);
}

// from
// tests/test_recorder_postroll.py::test_only_a_recording_can_pause_and_the_next_one_starts_unpaused
TEST_CASE("only a recording can pause and the next one starts unpaused",
          "[test_recorder_postroll]") {
    Rig rig;
    rig.make();
    CHECK_FALSE(rig.rec().pause());
    REQUIRE(rig.rec().start_recording());
    CHECK(rig.rec().pause());
    rig.rec().stop_recording();
    rig.rec().wait_for_stop_completion();

    REQUIRE(rig.rec().start_recording());
    CHECK_FALSE(rig.rec().paused());
}

// from tests/test_cancel_after_stop.py::test_recorder_cancel_ends_post_roll_and_keeps_nothing_after
TEST_CASE("recorder cancel ends post roll and keeps nothing after", "[test_cancel_after_stop]") {
    Rig rig;
    rig.make(RecorderOptions{.output_file = rig.file("canceled.wav")});
    REQUIRE(rig.rec().start_recording());
    feed(rig, repeat(-25.0, 20));
    const auto started = Clock::now();

    rig.rec().cancel_recording();
    // Blocks still arriving while the stream closes are dropped.
    device(rig).deliver(noise_block(-25.0, 99));
    REQUIRE(rig.rec().sync_capture());

    CHECK(rig.rec().capture_canceled());
    CHECK_FALSE(rig.rec().has_recording_data());
    REQUIRE(rig.rec().wait_for_stop_completion(std::chrono::milliseconds(1000)));
    CHECK(seconds_since(started) < 0.5); // not the 1.2 s cap
    CHECK_FALSE(rig.rec().is_recording());

    REQUIRE(rig.rec().start_recording()); // the next recording keeps audio again
    CHECK_FALSE(rig.rec().capture_canceled());
    device(rig).deliver(noise_block(-25.0, 100));
    REQUIRE(rig.rec().sync_capture());
    CHECK(rig.rec().has_recording_data());
}
