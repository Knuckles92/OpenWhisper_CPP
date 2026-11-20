// Recorder start, stop, data, save and level, over audio::testing::FakeBackend
// (tests/test_recorder.py, tests/test_recording_isolation.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <iterator>
#include <latch>
#include <system_error>
#include <thread>
#include <vector>

#include "audio/devices.h"
#include "audio/testing/origination_stamp.h"
#include "audio/wav_metadata.h"
#include "base/testing/fake_clock.h"
#include "recorder_test_support.h"

namespace {

using namespace fv::recorder_test;
using fv::Recorder;
using fv::RecorderOptions;
using std::chrono::milliseconds;

// The default stream the recorder opened: Python's _audio_callback fed directly.
void feed(Rig& rig, const std::vector<std::int16_t>& audio) {
    if (!rig.rec().journal()) {
        REQUIRE(rig.rec().start_recording());
    }
    rig.backend.opened().back().fake->deliver(audio);
    REQUIRE(rig.rec().sync_capture());
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::int16_t> first(const std::vector<std::int16_t>& samples, std::size_t count) {
    return {samples.begin(),
            samples.begin() + static_cast<std::ptrdiff_t>(std::min(count, samples.size()))};
}

} // namespace

// from tests/test_recorder.py::TestAudioRecorder::test_initialization
TEST_CASE("initialization", "[test_recorder]") {
    Rig rig;
    Recorder& recorder = rig.make();
    CHECK_FALSE(recorder.is_recording());
    CHECK_FALSE(recorder.has_recording_data());
    CHECK(recorder.chunk() == 1024);
    CHECK(recorder.channels() == 1);
    CHECK(recorder.rate() == 44100);
    CHECK(recorder.output_file() == rig.paths.user_file("recorded_audio.wav"));
}

// from tests/test_recorder.py::TestAudioRecorder::test_start_recording
TEST_CASE("start recording", "[test_recorder]") {
    Rig rig;
    Recorder& recorder = rig.make();
    CHECK(recorder.start_recording());
    CHECK(recorder.is_recording());
    CHECK_FALSE(recorder.has_recording_data());
}

// from tests/test_recorder.py::TestAudioRecorder::test_start_recording_already_recording
TEST_CASE("start recording already recording", "[test_recorder]") {
    Rig rig;
    Recorder& recorder = rig.make();
    REQUIRE(recorder.start_recording());
    CHECK_FALSE(recorder.start_recording());
    CHECK(rig.backend.opened().size() == 1);
}

// from
// tests/test_recorder.py::TestAudioRecorder::test_start_recording_fails_when_stream_cannot_open
TEST_CASE("start recording fails when stream cannot open", "[test_recorder]") {
    // No device at all: the default input cannot be queried.
    Rig rig(std::make_shared<fv::audio::testing::FakeBackend>());
    Recorder& recorder = rig.make();
    CHECK_FALSE(recorder.start_recording());
    CHECK_FALSE(recorder.is_recording());
    CHECK(recorder.last_start_error() == "No audio device available");
    CHECK(rig.backend.fake().open_streams() == 0);
    CHECK_FALSE(recorder.journal());
}

// from tests/test_recorder.py::TestAudioRecorder::test_stop_recording
TEST_CASE("stop recording", "[test_recorder]") {
    Rig rig;
    Recorder& recorder = rig.make();
    REQUIRE(recorder.start_recording());
    CHECK(recorder.stop_recording());
}

// from tests/test_recorder.py::TestAudioRecorder::test_stop_recording_not_recording
TEST_CASE("stop recording not recording", "[test_recorder]") {
    Rig rig;
    CHECK_FALSE(rig.make().stop_recording());
}

// from tests/test_recorder.py::TestAudioRecorder::test_has_recording_data
TEST_CASE("has recording data", "[test_recorder]") {
    Rig rig;
    rig.make();
    CHECK_FALSE(rig.rec().has_recording_data());
    feed(rig, arange(16));
    CHECK(rig.rec().has_recording_data());
}

// from tests/test_recorder.py::TestAudioRecorder::test_clear_recording_data
TEST_CASE("clear recording data", "[test_recorder]") {
    Rig rig;
    rig.make();
    feed(rig, arange(16));
    rig.rec().clear_recording_data();
    CHECK_FALSE(rig.rec().has_recording_data());
}

// from tests/test_recorder.py::TestAudioRecorder::test_get_recording_duration
TEST_CASE("get recording duration", "[test_recorder]") {
    Rig rig;
    rig.make();
    CHECK(rig.rec().get_recording_duration() == 0.0);
    feed(rig, arange(1000));
    CHECK(rig.rec().get_recording_duration() == 1000.0 / 44100.0);
}

// from tests/test_recorder.py::TestAudioRecorder::test_save_recording_no_data
TEST_CASE("save recording no data", "[test_recorder]") {
    Rig rig;
    rig.make();
    const auto target = rig.file("test_audio.wav");
    CHECK_FALSE(rig.rec().save_recording(target));
    CHECK_FALSE(std::filesystem::exists(target));
}

// from tests/test_recorder.py::TestAudioRecorder::test_save_recording_with_data
TEST_CASE("save recording with data", "[test_recorder]") {
    using namespace std::chrono;
    // datetime.now() pinned to 2026-09-19 13:05:07 local time.
    fv::FakeClock clock(sys_days{year{2026} / 9 / 19} + hours{13} + minutes{5} + seconds{7});
    Rig rig;
    RecorderOptions options;
    options.clock = &clock;
    rig.make(options);
    const auto audio = arange(500);
    feed(rig, audio);
    const auto target = rig.file("test_audio.wav");

    REQUIRE(rig.rec().save_recording(target));

    REQUIRE(std::filesystem::exists(target));
    CHECK(fv::audio::is_mono_pcm16(target));
    const auto bytes = read_bytes(target);
    CHECK(bytes[24] == 0x44); // 44100 Hz
    CHECK(bytes[25] == 0xAC);
    CHECK(first(wav_samples(target), audio.size()) == audio);
    CHECK(fv::audio::read_info_icrd(target) == "2026-09-19");
    CHECK(fv::audio::read_info_isft(target) == std::string(fv::identity().product));
    CHECK(fv::audio::read_bext_origination(target) ==
          std::pair<std::string, std::string>{"2026-09-19", "13:05:07"});
}

// from tests/test_recorder.py::TestAudioRecorder::test_capture_loss_requires_explicit_partial_save
TEST_CASE("capture loss requires explicit partial save", "[test_recorder]") {
    Rig rig;
    rig.make(RecorderOptions{.output_file = rig.file("test_audio.wav")});
    const auto audio = arange(500);
    feed(rig, audio);
    // Blocks that arrive after the journal stopped are lost (Python sets dropped_frames = 128).
    REQUIRE(rig.rec().journal()->finish());
    rig.backend.opened().back().fake->deliver(tone(9, 128));
    REQUIRE(rig.rec().sync_capture());
    REQUIRE(rig.rec().dropped_frames() == 128);

    const auto target = rig.file("test_audio.wav");
    CHECK_FALSE(rig.rec().save_recording(target));
    CHECK_FALSE(rig.rec().last_capture_error().empty());
    CHECK_FALSE(std::filesystem::exists(target));
    CHECK(std::filesystem::exists(rig.rec().journal()->path()));

    CHECK(rig.rec().save_recording(target, true));
    CHECK(first(wav_samples(target), audio.size()) == audio);
}

// from
// tests/test_recorder.py::TestAudioRecorder::test_storage_error_refuses_even_partial_save_and_keeps_journal
TEST_CASE("storage error refuses even partial save and keeps journal", "[test_recorder]") {
    Rig rig;
    rig.make(RecorderOptions{.output_file = rig.file("test_audio.wav")});
    feed(rig, arange(500));
    rig.rec().journal()->report_error("Storage stopped writing");
    const auto target = rig.file("test_audio.wav");
    CHECK_FALSE(rig.rec().save_recording(target, true));
    CHECK(rig.rec().last_capture_error() == "Storage stopped writing");
    CHECK_FALSE(std::filesystem::exists(target));
    CHECK(std::filesystem::exists(rig.rec().journal()->path()));
}

// from
// tests/test_recorder.py::TestAudioRecorder::test_save_recording_keeps_wav_when_metadata_stamp_fails
TEST_CASE("save recording keeps wav when metadata stamp fails", "[test_recorder]") {
    Rig rig;
    rig.make();
    const auto audio = arange(32);
    feed(rig, audio);
    const auto target = rig.file("test_audio.wav");
    // Python patches services.recorder.append_origination with OSError("disk full").
    struct FailingStamp {
        FailingStamp()
            : previous(fv::audio::testing::exchange_origination_stamp(
                  [](fv::audio::RiffFile&, std::chrono::local_seconds) {
                      throw std::system_error(std::make_error_code(std::errc::no_space_on_device),
                                              "disk full");
                  })) {}
        ~FailingStamp() { fv::audio::testing::exchange_origination_stamp(std::move(previous)); }
        fv::audio::testing::OriginationStamp previous;
    } failing;
    LogCapture logs;

    REQUIRE(rig.rec().save_recording(target));

    CHECK(first(wav_samples(target), audio.size()) == audio);
    CHECK_FALSE(fv::audio::read_info_icrd(target).has_value());
    CHECK(logs.starting_with("Failed to stamp WAV origination metadata").size() == 1);
}

// from tests/test_recorder.py::TestAudioRecorder::test_save_recording_writes_the_tagged_wav_once
TEST_CASE("save recording writes the tagged wav once", "[test_recorder]") {
    // Python counts os.replace and os.fsync calls; here the save leaves exactly the tagged
    // target beside the journal folder, and no temporary file.
    Rig rig;
    rig.make();
    feed(rig, arange(500));
    REQUIRE(rig.rec().journal()->finish());
    const auto folder = rig.root.path() / "out";
    std::filesystem::create_directories(folder);
    const auto target = folder / "test_audio.wav";
    REQUIRE(rig.rec().save_recording(target));

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        files.push_back(entry.path());
    }
    CHECK(files == std::vector<std::filesystem::path>{target});
    CHECK(fv::audio::read_info_icrd(target).has_value());
}

// from tests/test_recorder.py::TestAudioRecorder::test_save_recording_default_filename
TEST_CASE("save recording default filename", "[test_recorder]") {
    Rig rig;
    rig.make();
    feed(rig, arange(32));
    CHECK(rig.rec().save_recording());
    CHECK(std::filesystem::exists(rig.paths.user_file("recorded_audio.wav")));
}

// from tests/test_recorder.py::TestAudioRecorder::test_audio_level_callback
TEST_CASE("audio level callback", "[test_recorder]") {
    Rig rig;
    rig.make();
    std::vector<float> values;
    const auto subscription =
        rig.rec().level.connect(rig.inline_executor, [&](float level) { values.push_back(level); });
    feed(rig, {1000, -1000, 2000, -2000});
    REQUIRE(values.size() == 1);
    CHECK(values[0] >= 0.0f);
    CHECK(values[0] <= 1.0f);
    // 0.7 * RMS / 32767 from a level of 0: the smoothing of config.WAVEFORM_LEVEL_SMOOTHING.
    CHECK_THAT(values[0], Catch::Matchers::WithinAbs(0.7 * std::sqrt(2.5e6) / 32767.0, 1e-6));
}

// from tests/test_recorder.py::TestAudioRecorder::test_audio_callback
TEST_CASE("audio callback", "[test_recorder]") {
    Rig rig;
    rig.make();
    const std::vector<std::int16_t> audio{100, -100, 200, -200};
    feed(rig, audio);
    CHECK(rig.rec().has_recording_data());
    CHECK(rig.rec().snapshot().recorded_bytes == 8);
    CHECK(rig.rec().read_recorded_bytes(0) == string_of(audio));
    CHECK(rig.rec().read_recorded_bytes(4) == string_of({200, -200}));
    CHECK_FALSE(rig.rec().read_recorded_bytes(10).has_value());
    REQUIRE(rig.rec().tape());
    CHECK(rig.rec().tape()->frames() == 4);
}

// from
// tests/test_recorder.py::TestAudioRecorder::test_recording_is_journaled_to_disk_and_streams_to_wav
TEST_CASE("recording is journaled to disk and streams to wav", "[test_recorder]") {
    Rig rig;
    Recorder& recorder = rig.make(RecorderOptions{.output_file = rig.file("test_audio.wav")});
    REQUIRE(recorder.start_recording());
    const auto audio = arange(256);
    rig.backend.opened().back().fake->deliver(audio);
    REQUIRE(recorder.save_recording());
    const auto journaled = read_bytes(recorder.journal()->path());
    const auto expected = bytes_of(audio);
    CHECK(journaled.size() == expected.size());
    CHECK(
        std::equal(journaled.begin(), journaled.end(), expected.begin(),
                   [](std::uint8_t a, std::byte b) { return a == static_cast<std::uint8_t>(b); }));
    CHECK(first(wav_samples(rig.file("test_audio.wav")), audio.size()) == audio);
}

// from
// tests/test_recording_isolation.py::test_default_recorder_test_preserves_preexisting_recording
TEST_CASE("default recorder test preserves preexisting recording", "[test_recording_isolation]") {
    // A recording outside the recorder's data root is never touched by its start or default
    // save.
    const fv::testkit::TempDataRoot elsewhere;
    const auto sentinel = elsewhere.path() / "recorded_audio.wav";
    {
        std::ofstream out(sentinel, std::ios::binary);
        out << "existing recording";
    }
    {
        Rig rig;
        rig.make();
        feed(rig, arange(32));
        REQUIRE(rig.rec().save_recording());
        CHECK(rig.rec().output_file().parent_path() == rig.root.path());
    }
    const auto bytes = read_bytes(sentinel);
    CHECK(std::string(bytes.begin(), bytes.end()) == "existing recording");
}

TEST_CASE("the saved wav carries the end padding", "[test_recorder]") {
    Rig rig;
    rig.make();
    feed(rig, tone(7, 1024));
    REQUIRE(rig.rec().save_recording());
    const auto samples = wav_samples(rig.paths.user_file("recorded_audio.wav"));
    // 500 ms of silence after the audio (config.END_PADDING_MS).
    REQUIRE(samples.size() == 1024 + 22050);
    CHECK(std::all_of(samples.begin(), samples.begin() + 1024, [](auto s) { return s == 7; }));
    CHECK(std::all_of(samples.begin() + 1024, samples.end(), [](auto s) { return s == 0; }));
}

TEST_CASE("the device callback only copies into the ring", "[test_recorder]") {
    // Architecture section 4 rule 4: no allocation on the device thread, flagged or not.
    Rig rig;
    rig.make();
    REQUIRE(rig.rec().start_recording());
    auto& device = *rig.backend.opened().back().fake;
    const auto block = tone(11);
    {
        const fv::testkit::RealtimeGuard guard;
        device.deliver(block);
        device.deliver(block, fv::audio::BlockStatus{.overflow = true});
    }
    REQUIRE(rig.rec().pause());
    {
        const fv::testkit::RealtimeGuard guard;
        device.deliver(block);
    }
    rig.rec().resume();
    REQUIRE(rig.rec().sync_capture());
    CHECK(rig.rec().snapshot().recorded_frames == 2 * 1024);
    CHECK(rig.rec().input_overflows() == 1);
}

TEST_CASE("concurrent starts open one stream", "[test_recorder]") {
    // The facade is called from the UI executor and workers: two starts racing must not both
    // open a microphone and orphan one session.
    Rig rig;
    rig.make();
    rig.backend.before_open(
        [](int) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
    std::latch go(2);
    const auto start = [&] {
        go.arrive_and_wait();
        return rig.rec().start_recording();
    };
    auto first = std::async(std::launch::async, start);
    auto second = std::async(std::launch::async, start);
    const int started = static_cast<int>(first.get()) + static_cast<int>(second.get());
    CHECK(started == 1);
    CHECK(rig.backend.opened().size() == 1);
    CHECK(rig.backend.fake().open_streams() == 1);
    rig.rec().cleanup();
    CHECK(rig.backend.fake().open_streams() == 0);
}

TEST_CASE("start error messages", "[test_recorder]") {
    CHECK(Recorder::format_start_error("Error querying device -1") == "No audio device available");
    CHECK(Recorder::format_start_error("PORTAUDIO is down") == "No audio device available");
    CHECK(Recorder::format_start_error("Permission denied") == "Permission denied");
    CHECK(Recorder::format_start_error("") == "Could not open the audio stream");
}

TEST_CASE("input devices are listed or logged empty", "[test_recorder]") {
    auto fake = windows_fake();
    CHECK(Recorder::get_input_devices(*fake).size() == 7); // every input but the loopback
    fake->fail_listing("PortAudio not initialized");
    LogCapture logs;
    CHECK(Recorder::get_input_devices(*fake).empty());
    CHECK(logs.starting_with("Failed to enumerate audio devices: ").size() == 1);
}

// Acceptance: two seconds from the default microphone, saved with its tags and end padding.
// Opens the real microphone, so only the owner runs it (windows-interactive).
TEST_CASE("a two second recording from the default microphone", "[interactive]") {
    const fv::testkit::TempDataRoot root;
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    const auto backend = fv::audio::make_portaudio_backend();
    Recorder recorder(*backend, paths);
    REQUIRE(recorder.start_recording());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    REQUIRE(recorder.stop_recording());
    REQUIRE(recorder.wait_for_stop_completion());
    REQUIRE(recorder.save_recording());
    const auto saved = paths.user_file("recorded_audio.wav");
    CHECK(fv::audio::is_mono_pcm16(saved));
    CHECK(fv::audio::read_info_isft(saved) == std::string(fv::identity().product));
    CHECK(fv::audio::read_bext_origination(saved).has_value());
    const auto samples = wav_samples(saved);
    CHECK(samples.size() >= 2 * 44100 + 22050);
    CHECK(std::all_of(samples.end() - 22050, samples.end(), [](auto s) { return s == 0; }));
    recorder.acknowledge_recording();
}
