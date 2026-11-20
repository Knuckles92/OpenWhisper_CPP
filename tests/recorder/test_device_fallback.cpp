// Dictation starts on the first ranked microphone that opens and moves on if it is lost
// (tests/test_personalize_s7_recorder.py, tests/test_personalize_fix_f3_lost_mic_close.py).
// The fake device list is OpenWhisper's measured Windows layout; streams deliver blocks only when
// a test feeds them, so no microphone is opened. The recorder's own watcher still detects the
// loss and switches, exactly as in the app.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/executor.h"
#include "recorder_test_support.h"
#include "settings/testing/memory_store.h"

namespace {

using namespace fv::recorder_test;
using fv::Json;
using fv::Recorder;
using fv::RecorderOptions;
using std::chrono::milliseconds;

constexpr std::size_t kChunk = 1024;
constexpr std::size_t kGapFrames = 44100 * 250 / 1000;

// What the recorder reported: switches and failures, as test_personalize_s7_recorder.py's
// _watch collects them.
struct Watch {
    explicit Watch(Rig& rig)
        : switch_link(rig.rec().device_switched.connect(
              rig.inline_executor,
              [this](const std::string& from, const std::string& to) {
                  {
                      const std::lock_guard lock(mutex);
                      switches.emplace_back(from, to);
                  }
                  switched.set();
              })),
          error_link(rig.rec().capture_error.connect(rig.inline_executor,
                                                     [this](const std::string& message) {
                                                         {
                                                             const std::lock_guard lock(mutex);
                                                             errors.push_back(message);
                                                         }
                                                         failed.set();
                                                     })) {}

    std::vector<std::pair<std::string, std::string>> switch_list() {
        const std::lock_guard lock(mutex);
        return switches;
    }
    std::vector<std::string> error_list() {
        const std::lock_guard lock(mutex);
        return errors;
    }

    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> switches;
    std::vector<std::string> errors;
    Flag switched;
    Flag failed;
    fv::Subscription switch_link;
    fv::Subscription error_link;
};

Recorder& ranked(Rig& rig, std::vector<fv::audio::DeviceKey> priority = {kUsb, kSnowball},
                 RecorderOptions options = {}) {
    options.output_file = rig.file("dictation.wav");
    options.device_priority = std::move(priority);
    return rig.make(std::move(options));
}

std::vector<std::int16_t> saved_samples(Rig& rig) {
    REQUIRE(rig.rec().save_recording());
    return wav_samples(rig.rec().output_file());
}

bool all_equal(const std::vector<std::int16_t>& samples, std::size_t from, std::size_t to,
               std::int16_t value) {
    if (samples.size() < to) {
        return false;
    }
    return std::all_of(samples.begin() + static_cast<std::ptrdiff_t>(from),
                       samples.begin() + static_cast<std::ptrdiff_t>(to),
                       [value](std::int16_t s) { return s == value; });
}

void finish(Rig& rig) {
    // Python stops, ends the post-roll by hand and waits; here the post-roll ends at a short
    // cap.
    rig.rec().stop_recording();
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));
}

RecorderOptions short_post_roll() {
    RecorderOptions options;
    options.timings.post_roll = milliseconds(20);
    return options;
}

} // namespace

// from tests/test_personalize_s7_recorder.py::test_start_moves_past_a_microphone_that_will_not_open
TEST_CASE("start moves past a microphone that will not open", "[test_personalize_s7_recorder]") {
    Rig rig;
    rig.backend.refuse({2});
    ranked(rig);

    REQUIRE(rig.rec().start_recording());

    CHECK(rig.backend.opened_devices() == std::vector<int>{1});
    REQUIRE(rig.rec().active_device().has_value());
    CHECK(rig.rec().active_device()->index == 1);
    CHECK(rig.backend.fake().open_streams() == 1);
}

// from tests/test_personalize_s7_recorder.py::test_wasapi_microphones_open_with_conversion
TEST_CASE("wasapi microphones open with conversion", "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig, {{"Microphone (2- USB Audio Device)", "Windows WASAPI"}});

    REQUIRE(rig.rec().start_recording());

    const auto config = rig.backend.stream(0).fake->config();
    CHECK(config.device_index == 5);
    CHECK(config.wasapi_auto_convert);
    CHECK(config.rate == 44100);
    CHECK(config.frames_per_block == 1024);
}

// from
// tests/test_personalize_s7_recorder.py::test_without_a_ranked_microphone_start_queries_no_driver
TEST_CASE("without a ranked microphone start queries no driver", "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig, {});

    REQUIRE(rig.rec().start_recording());

    CHECK(rig.backend.fake().listings() == 0);
    CHECK(rig.backend.opened_devices() == std::vector<int>{-1});
    CHECK_FALSE(rig.rec().active_device().has_value());
}

// from tests/test_personalize_s7_recorder.py::test_when_nothing_opens_start_reports_no_device
TEST_CASE("when nothing opens start reports no device", "[test_personalize_s7_recorder]") {
    Rig rig;
    rig.backend.refuse({-1, 0, 1, 2});
    ranked(rig);

    CHECK_FALSE(rig.rec().start_recording());

    CHECK(rig.rec().last_start_error() == "No audio device available");
    CHECK(rig.backend.fake().open_streams() == 0);
}

// from
// tests/test_personalize_s7_recorder.py::test_a_lost_microphone_hands_the_same_recording_to_the_next
TEST_CASE("a lost microphone hands the same recording to the next",
          "[test_personalize_s7_recorder]") {
    MetricsCapture metrics;
    Rig rig;
    ranked(rig, {kUsb, kSnowball}, short_post_roll());
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);
    usb.fake->deliver(tone(100));

    usb.control->inactive.store(true); // unplugged
    REQUIRE(watch.switched.wait(milliseconds(3000)));

    const auto snowball = rig.backend.stream(1);
    CHECK(snowball.device == 1);
    CHECK(snowball.fake->started());
    CHECK_FALSE(snowball.fake->aborted());
    CHECK(usb.fake->aborted());
    CHECK(usb.fake->closed());
    CHECK(watch.switch_list() ==
          std::vector<std::pair<std::string, std::string>>{{"USB Audio Device", "Blue Snowball"}});
    usb.fake->deliver(tone(999)); // a late block from the dead stream
    snowball.fake->deliver(tone(200));
    REQUIRE(rig.rec().sync_capture());
    // A fresh gate: two microphones' floors would make a meaningless threshold.
    CHECK(rig.rec().snapshot().post_roll_gate.frames_before_stop() == 1024);
    finish(rig);

    const auto samples = saved_samples(rig);
    CHECK(all_equal(samples, 0, kChunk, 100));
    CHECK(all_equal(samples, kChunk, kChunk + kGapFrames, 0));
    CHECK(all_equal(samples, kChunk + kGapFrames, 2 * kChunk + kGapFrames, 200));
    CHECK(watch.error_list().empty());
    CHECK(rig.rec().dropped_frames() == 0);
    CHECK(rig.rec().last_capture_error().empty());
    CHECK(metrics.last("device_switches") == 1.0);
    rig.rec().cleanup();
    CHECK(rig.backend.fake().open_streams() == 0);
}

// from
// tests/test_personalize_s7_recorder.py::test_blocks_from_the_replacement_never_land_before_the_gap
TEST_CASE("blocks from the replacement never land before the gap",
          "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig, {kUsb, kSnowball}, short_post_roll());
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);
    usb.fake->deliver(tone(100));
    // A device may deliver a block before the switch has finished.
    rig.backend.on_start([](fv::audio::testing::FakeStream& stream) { stream.deliver(tone(300)); });

    usb.control->inactive.store(true);
    REQUIRE(watch.switched.wait(milliseconds(3000)));
    rig.backend.stream(1).fake->deliver(tone(200));
    finish(rig);

    const auto samples = saved_samples(rig);
    CHECK(std::find(samples.begin(), samples.end(), std::int16_t{300}) == samples.end());
    CHECK(all_equal(samples, kChunk, kChunk + kGapFrames, 0));
    CHECK(all_equal(samples, kChunk + kGapFrames, 2 * kChunk + kGapFrames, 200));
}

// from
// tests/test_personalize_s7_recorder.py::test_a_finished_stream_counts_as_lost_but_our_own_close_does_not
TEST_CASE("a finished stream counts as lost but our own close does not",
          "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig, {kUsb, kSnowball}, short_post_roll());
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);
    usb.fake->deliver(tone(1));

    usb.fake->lose();
    REQUIRE(watch.switched.wait(milliseconds(3000)));
    const auto snowball = rig.backend.stream(1);
    snowball.fake->deliver(tone(2));
    finish(rig);

    snowball.fake->lose(); // the device reports our own abort too
    std::this_thread::sleep_for(milliseconds(300));
    CHECK(watch.switch_list().size() == 1);
    CHECK(rig.backend.opened().size() == 2);
}

// from tests/test_personalize_s7_recorder.py::test_blocks_stopping_counts_as_lost
TEST_CASE("blocks stopping counts as lost", "[test_personalize_s7_recorder]") {
    Rig rig;
    RecorderOptions options;
    options.timings.stall_after_blocks = milliseconds(300);
    ranked(rig, {kUsb, kSnowball}, options);
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    rig.backend.stream(0).fake->deliver(tone(5));

    REQUIRE(watch.switched.wait(milliseconds(3000))); // no block for longer than the limit
    CHECK(rig.backend.opened_devices() == std::vector<int>{2, 1});
}

// from
// tests/test_personalize_s7_recorder.py::test_a_lone_overflow_keeps_its_block_and_the_recording
TEST_CASE("a lone overflow keeps its block and the recording", "[test_personalize_s7_recorder]") {
    MetricsCapture metrics;
    Rig rig;
    ranked(rig, {kUsb, kSnowball}, short_post_roll());
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);

    usb.fake->deliver(tone(5), fv::audio::BlockStatus{.overflow = true});
    usb.fake->deliver(tone(6));
    finish(rig);

    const auto samples = saved_samples(rig);
    CHECK(all_equal(samples, 0, kChunk, 5));
    CHECK(all_equal(samples, kChunk, 2 * kChunk, 6));
    CHECK(watch.error_list().empty());
    CHECK(rig.rec().dropped_frames() == 0);
    CHECK(rig.rec().last_capture_error().empty());
    CHECK(metrics.last("input_overflows") == 1.0);
    CHECK(watch.switch_list().empty());
    CHECK(rig.backend.opened().size() == 1);
}

// from
// tests/test_personalize_s7_recorder.py::test_overflows_that_keep_coming_fail_the_recording_without_switching
TEST_CASE("overflows that keep coming fail the recording without switching",
          "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig);
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);

    for (int i = 0; i < 3; ++i) {
        usb.fake->deliver(tone(5), fv::audio::BlockStatus{.overflow = true});
    }

    REQUIRE(watch.failed.wait(milliseconds(3000)));
    REQUIRE_FALSE(watch.error_list().empty());
    CHECK(watch.error_list()[0].find("overflowing") != std::string::npos);
    CHECK(watch.switch_list().empty());
    CHECK(rig.backend.opened().size() == 1);
}

// from tests/test_personalize_s7_recorder.py::test_overflows_spread_beyond_the_window_never_fail
TEST_CASE("overflows spread beyond the window never fail", "[test_personalize_s7_recorder]") {
    Rig rig;
    RecorderOptions options = short_post_roll();
    options.timings.overflow_window = milliseconds(50);
    ranked(rig, {kUsb, kSnowball}, options);
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);

    for (int i = 0; i < 4; ++i) {
        usb.fake->deliver(tone(5), fv::audio::BlockStatus{.overflow = true});
        REQUIRE(rig.rec().sync_capture());
        std::this_thread::sleep_for(milliseconds(200));
    }
    finish(rig);

    CHECK(watch.error_list().empty());
    CHECK(rig.rec().input_overflows() == 4);
}

// from
// tests/test_personalize_s7_recorder.py::test_any_other_status_still_fails_the_recording_at_once
TEST_CASE("any other status still fails the recording at once", "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig);
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());

    rig.backend.stream(0).fake->deliver(tone(5), fv::audio::BlockStatus{.underflow = true});

    REQUIRE(watch.failed.wait(milliseconds(3000)));
    CHECK(watch.error_list() ==
          std::vector<std::string>{"Audio input error (input underflow); recording stopped."});
    CHECK(watch.switch_list().empty());
    CHECK(rig.backend.opened().size() == 1);
}

TEST_CASE("a block flagged both ways names both flags", "[test_personalize_s7_recorder]") {
    // sounddevice's CallbackFlags.__str__ lists the set flags in dir() order.
    Rig rig;
    ranked(rig);
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());

    rig.backend.stream(0).fake->deliver(
        tone(5), fv::audio::BlockStatus{.overflow = true, .underflow = true});

    REQUIRE(watch.failed.wait(milliseconds(3000)));
    CHECK(watch.error_list() ==
          std::vector<std::string>{
              "Audio input error (input overflow, input underflow); recording stopped."});
    CHECK(rig.rec().input_overflows() == 0);
}

// from tests/test_personalize_s7_recorder.py::test_no_switch_after_the_stop_press
TEST_CASE("no switch after the stop press", "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig);
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);
    usb.fake->deliver(tone(5));
    REQUIRE(rig.rec().stop_recording());

    usb.control->inactive.store(true);
    std::this_thread::sleep_for(milliseconds(300));
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));

    CHECK(watch.switch_list().empty());
    CHECK(watch.error_list().empty());
    CHECK(rig.backend.opened().size() == 1);
}

// from
// tests/test_personalize_s7_recorder.py::test_a_cancel_while_switching_keeps_the_replacement_closed
TEST_CASE("a cancel while switching keeps the replacement closed",
          "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig);
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);
    usb.fake->deliver(tone(5));
    Flag canceled;
    rig.backend.before_open([&](int device) {
        if (device == 1 && !canceled.is_set()) {
            canceled.set();
            rig.rec().cancel_recording();
        }
    });

    usb.control->inactive.store(true);
    REQUIRE(canceled.wait(milliseconds(3000)));
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));

    CHECK(watch.switch_list().empty());
    CHECK(watch.error_list().empty());
    const auto replacement = rig.backend.opened().back();
    CHECK(replacement.device == 1);
    CHECK(replacement.fake->closed());
    CHECK(rig.backend.fake().open_streams() == 0);
}

// from
// tests/test_personalize_s7_recorder.py::test_with_no_other_microphone_the_partial_recording_fails_as_before
TEST_CASE("with no other microphone the partial recording fails as before",
          "[test_personalize_s7_recorder]") {
    Rig rig;
    ranked(rig);
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);
    usb.fake->deliver(tone(7));
    rig.backend.refuse({0, 1});

    usb.control->inactive.store(true);

    REQUIRE(watch.failed.wait(milliseconds(3000)));
    CHECK(watch.error_list() == std::vector<std::string>{std::string(fv::NO_MICROPHONE_LEFT)});
    CHECK(watch.switch_list().empty());
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));
    CHECK(rig.rec().save_recording(std::nullopt, true));
}

// from
// tests/test_personalize_s7_recorder.py::test_the_default_microphone_falls_back_to_the_sound_mapper
TEST_CASE("the default microphone falls back to the sound mapper",
          "[test_personalize_s7_recorder]") {
    if (!fv::audio::kSoundMapperPlatform) {
        SKIP("the sound mapper is a Windows (MME) device");
    }
    Rig rig;
    ranked(rig, {});
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    rig.backend.stream(0).fake->deliver(tone(3));

    rig.backend.stream(0).control->inactive.store(true);

    REQUIRE(watch.switched.wait(milliseconds(3000)));
    CHECK(rig.backend.opened_devices() == std::vector<int>{-1, 0});
    CHECK(watch.switch_list() == std::vector<std::pair<std::string, std::string>>{{"", ""}});
}

// from tests/test_personalize_s7_recorder.py::test_from_settings_uses_the_saved_order
TEST_CASE("from settings uses the saved order", "[test_personalize_s7_recorder]") {
    Rig rig;
    fv::MemorySettingsStore store(Json{
        {"audio_input_priority", Json::array({
                                     {{"name", kSnowball.name}, {"hostapi", kSnowball.hostapi}},
                                     {{"name", kUsb.name}, {"hostapi", kUsb.hostapi}},
                                 })}});
    fv::ThreadPool pool("fv-test-migration", 1);

    auto recorder = Recorder::from_settings(store, rig.backend, rig.paths, pool);

    CHECK(recorder->device_priority() == std::vector<fv::audio::DeviceKey>{kSnowball, kUsb});
    CHECK(recorder->output_file() == rig.paths.user_file("recorded_audio.wav"));
}

// from tests/test_personalize_s7_recorder.py::test_from_settings_migrates_the_legacy_index
TEST_CASE("from settings migrates the legacy index", "[test_personalize_s7_recorder]") {
    Rig rig;
    fv::MemorySettingsStore store(Json{{"audio_input_device", 2}});
    fv::ThreadPool pool("fv-test-migration", 1);

    auto recorder = Recorder::from_settings(store, rig.backend, rig.paths, pool);

    CHECK(wait_until(
        [&] { return recorder->device_priority() == std::vector<fv::audio::DeviceKey>{kUsb}; }));
    CHECK(store.contents()["audio_input_priority"] ==
          Json::array({{{"name", kUsb.name}, {"hostapi", kUsb.hostapi}}}));
}

// from
// tests/test_personalize_s7_recorder.py::test_start_falls_back_to_the_default_when_devices_cannot_be_listed
TEST_CASE("start falls back to the default when devices cannot be listed",
          "[test_personalize_s7_recorder]") {
    Rig rig;
    rig.backend.fake().fail_listing("PortAudio not initialized");
    ranked(rig);

    REQUIRE(rig.rec().start_recording());

    CHECK(rig.backend.opened_devices() == std::vector<int>{-1});
}

// --- a lost microphone whose driver hangs on close --------------------------------------------

namespace {

// tests/test_personalize_fix_f3_lost_mic_close.py: the next dictation can start.
void ready_for_the_next_dictation(Rig& rig, const OpenedStream& lost) {
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));
    CHECK_FALSE(rig.rec().is_recording());
    rig.backend.refuse({});
    rig.backend.before_open(nullptr);
    REQUIRE(rig.rec().start_recording());
    CHECK(rig.backend.opened().back().fake != lost.fake);
}

RecorderOptions hung_close() {
    RecorderOptions options;
    options.timings.lost_stream_close = milliseconds(200);
    return options;
}

} // namespace

// from
// tests/test_personalize_fix_f3_lost_mic_close.py::test_with_no_other_microphone_a_hung_close_still_ends_the_recording
TEST_CASE("with no other microphone a hung close still ends the recording",
          "[test_personalize_fix_f3_lost_mic_close]") {
    Rig rig;
    ranked(rig, {kUsb, kSnowball}, hung_close());
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);
    usb.control->hang.store(true);
    usb.fake->deliver(tone(7));
    rig.backend.refuse({0, 1});

    usb.control->inactive.store(true);

    REQUIRE(watch.failed.wait(milliseconds(3000)));
    CHECK(watch.error_list() == std::vector<std::string>{std::string(fv::NO_MICROPHONE_LEFT)});
    REQUIRE(usb.control->abort_entered.wait(milliseconds(3000)));
    ready_for_the_next_dictation(rig, usb);
    usb.control->release.set();
    rig.rec().cleanup();
    CHECK(wait_until([&] { return usb.control->is_closed(); }));
}

// from
// tests/test_personalize_fix_f3_lost_mic_close.py::test_ending_during_the_switch_with_a_hung_close_still_ends_the_recording
TEST_CASE("ending during the switch with a hung close still ends the recording",
          "[test_personalize_fix_f3_lost_mic_close]") {
    const bool cancel = GENERATE(true, false);
    Rig rig;
    ranked(rig, {kUsb, kSnowball}, hung_close());
    Watch watch(rig);
    REQUIRE(rig.rec().start_recording());
    const auto usb = rig.backend.stream(0);
    usb.control->hang.store(true);
    usb.fake->deliver(tone(5));
    Flag ended;
    rig.backend.before_open([&](int device) {
        if (device == 1 && !ended.is_set()) {
            ended.set();
            if (cancel) {
                rig.rec().cancel_recording();
            } else {
                rig.rec().stop_recording();
            }
        }
    });

    usb.control->inactive.store(true);

    REQUIRE(ended.wait(milliseconds(3000)));
    REQUIRE(usb.control->abort_entered.wait(milliseconds(3000)));
    REQUIRE(rig.rec().wait_for_stop_completion(milliseconds(3000)));
    CHECK(watch.switch_list().empty());
    CHECK(watch.error_list().empty());
    CHECK(rig.backend.opened().back().fake->closed());
    ready_for_the_next_dictation(rig, usb);
    usb.control->release.set();
    rig.rec().cleanup();
    CHECK(wait_until([&] { return usb.control->is_closed(); }));
}
