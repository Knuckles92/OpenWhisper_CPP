// Ported from OpenWhisper tests/test_meeting_sd_stream.py: capture timestamps never turn valid
// microphone buffers into gaps. The device is audio::testing::FakeBackend, whose BlockStatus
// carries the two callback times PortAudio's time_info does.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "audio/testing/fake_backend.h"
#include "base/testing/fake_clock.h"
#include "capture_test_support.h"
#include "meeting_capture/detail/wav_file.h"
#include "meeting_capture/health.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_capture/sources.h"
#include "meeting_capture/spool.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "testkit/testkit.h"

using Catch::Approx;
using fv::audio::BlockStatus;
using fv::audio::testing::FakeBackend;
using fv::audio::testing::FakeStream;
using fv::meeting::CaptureBlock;
using fv::meeting::MeetingClock;
using fv::meeting::MicSource;
using fv::meeting::test::set_steady;

namespace {

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// _source(): the mic channel on device 1 at 44.1 kHz mono, its blocks collected.
struct Rig {
    Rig() {
        backend->set_layout(
            {{.name = "MME", .default_input_device = 1}},
            {{.name = "Microsoft Sound Mapper - Input", .hostapi = 0, .max_input_channels = 2},
             {.name = "Microphone (Blue Snowball )", .hostapi = 0, .max_input_channels = 1}},
            1);
        set_steady(fake, 1000.0);
    }
    std::shared_ptr<FakeStream> stream() const { return backend->streams().back(); }

    std::shared_ptr<FakeBackend> backend = std::make_shared<FakeBackend>();
    fv::FakeClock fake;
    MeetingClock clock{fake};
};

} // namespace

// from tests/test_meeting_sd_stream.py::test_unavailable_callback_times_keep_monotonic_timestamp
TEST_CASE("unavailable callback times keep monotonic timestamp", "[test_meeting_sd_stream]") {
    const auto [adc, current] = GENERATE(table<double, double>({
        {0.0, 0.0},
        {1024.0 / 44100, 0.0}, // MME's second batched buffer: an offset from a zero clock
        {2.0, 1.0},
        {-1.0, 1.0},
        {kNan, 1.0},
        {1.0, kNan},
        {1.0, kInf},
    }));
    Rig rig;
    std::vector<double> stamps;
    MicSource source(rig.backend, rig.clock, "mic", 1, 44100, 1);
    source.start([&](const CaptureBlock& block) { stamps.push_back(block.t_mono); });
    const std::vector<std::int16_t> data(1024, 1000);
    rig.stream()->deliver(data, BlockStatus{.adc_time = adc, .current_time = current});
    REQUIRE(stamps.size() == 1);
    CHECK(stamps[0] == 1000.0);
    source.stop();
}

// from tests/test_meeting_sd_stream.py::test_valid_callback_clock_preserves_adc_latency
TEST_CASE("valid callback clock preserves adc latency", "[test_meeting_sd_stream]") {
    const auto [adc, current] = GENERATE(table<double, double>({{75.0, 75.04}, {0.0, 0.04}}));
    Rig rig;
    std::vector<double> stamps;
    MicSource source(rig.backend, rig.clock, "mic", 1, 44100, 1);
    source.start([&](const CaptureBlock& block) { stamps.push_back(block.t_mono); });
    const std::vector<std::int16_t> data(1024, 1000);
    rig.stream()->deliver(data, BlockStatus{.adc_time = adc, .current_time = current});
    REQUIRE(stamps.size() == 1);
    CHECK(stamps[0] == Approx(999.96));
    source.stop();
}

// from tests/test_meeting_sd_stream.py::test_mme_batched_callbacks_preserve_audio_through_spool
TEST_CASE("mme batched callbacks preserve audio through spool", "[test_meeting_sd_stream]") {
    // The Blue Snowball's MME clock at 44.1 kHz: two buffers delivered together, currentTime 0
    // for both, ADC times 0 and 1024/44100. Mixing them with another clock once dropped every
    // second buffer and replaced half the recording with silence.
    fv::meeting::test::TempDir folder;
    Rig rig;
    rig.clock.resume_from_recovery(0.0); // anchored at monotonic 1000.0
    auto repository = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(rig.fake);
    fv::meeting::SpoolOptions options;
    options.queue_size = 256;
    fv::meeting::SpoolWriter writer(
        "m_timing", "mic", folder.path(), rig.clock, repository,
        [](const fv::meeting_state::SpooledChunk&) {}, options);
    MicSource source(rig.backend, rig.clock, "mic", 1, 44100, 1);
    source.start([&](const CaptureBlock& block) { writer.feed(block); });
    constexpr std::size_t block = fv::meeting::MIC_BLOCK_FRAMES;
    const std::size_t count = 100 * block;
    // No source silence: any silent interval would be a capture defect.
    std::vector<std::int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<std::int16_t>(
            8000 + 2000 * std::sin(2 * 3.141592653589793 * 440 * static_cast<double>(i) / 44100));
    }
    for (std::size_t i = 0; i * block < count; ++i) {
        set_steady(rig.fake, 1000.2 + static_cast<double>(i / 2) * 2 * block / 44100.0);
        std::vector<std::int16_t> data(samples.begin() + static_cast<std::ptrdiff_t>(i * block),
                                       samples.begin() +
                                           static_cast<std::ptrdiff_t>((i + 1) * block));
        while (writer.queued_blocks() + 2 > writer.queue_capacity()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rig.stream()->deliver(data,
                              BlockStatus{.adc_time = static_cast<double>(i % 2) * block / 44100.0,
                                          .current_time = 0.0});
        std::fill(data.begin(), data.end(), 0); // PortAudio may reuse the buffer at once
    }
    source.stop();
    writer.flush();
    const auto [actual, rate] =
        fv::meeting::detail::load_wav_int16(folder.path() / "mic_session.wav");
    CHECK(rate == 16000);
    CHECK(actual == fv::meeting::resample_to_16k(samples, 44100));
    CHECK(writer.timeline_fixes() == 0);
}

TEST_CASE("a stereo microphone is averaged to mono through int32", "[test_meeting_sd_stream]") {
    Rig rig;
    std::vector<std::int16_t> mono;
    MicSource source(rig.backend, rig.clock, "mic", 0, 44100, 2);
    source.start(
        [&](const CaptureBlock& block) { mono.assign(block.frames, block.frames + block.count); });
    const std::vector<std::int16_t> stereo = {32767, 32767, -32768, -32767, 3, -6, -3, 0, 5, 6};
    rig.stream()->deliver(stereo);
    CHECK(mono == std::vector<std::int16_t>{32767, -32767, -1, -1, 5});
    source.stop();
}

TEST_CASE("the device callback allocates nothing", "[test_meeting_sd_stream]") {
    fv::meeting::test::TempDir folder;
    Rig rig;
    rig.clock.start();
    auto repository = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>(rig.fake);
    fv::meeting::SpoolWriter writer("m_rt", "mic", folder.path(), rig.clock, repository,
                                    [](const fv::meeting_state::SpooledChunk&) {});
    fv::meeting::CaptureSignalMonitor monitor(rig.fake);
    MicSource source(rig.backend, rig.clock, "mic", 0, 48000, 2);
    source.start([&](const CaptureBlock& block) {
        monitor.observe(block, true);
        writer.feed(block);
    });
    const std::vector<std::int16_t> stereo(2 * 1024, 1200);
    // Taken before the guard: streams() copies the fake's vector, which allocates on the test's
    // side, not the device callback's.
    const std::shared_ptr<FakeStream> stream = rig.stream();
    {
        const fv::testkit::RealtimeGuard guard;
        stream->deliver(stereo, BlockStatus{.adc_time = 1.0, .current_time = 1.01});
        stream->deliver(stereo);
    }
    source.stop();
    writer.flush();
}

TEST_CASE("a stream that ends by itself is inactive and says so once", "[test_meeting_sd_stream]") {
    Rig rig;
    const fv::meeting::test::LogCapture logs;
    MicSource source(rig.backend, rig.clock, "mic", 1, 44100, 1);
    source.start([](const CaptureBlock&) {});
    CHECK(source.is_active());
    CHECK(source.requires_audio_blocks());
    CHECK(std::get<int>(source.device_id()) == 1);
    rig.stream()->lose("unplugged");
    CHECK_FALSE(source.is_active());
    CHECK_FALSE(source.is_active());
    CHECK(logs.levels("Capture stream ended unexpectedly: channel=mic") ==
          std::vector{fv::log::Level::Warning});
}

TEST_CASE("a device that does not open raises out of start", "[test_meeting_sd_stream]") {
    Rig rig;
    rig.backend->fail_open(1, "Invalid device [PaErrorCode -9996]");
    MicSource source(rig.backend, rig.clock, "mic", 1, 44100, 1);
    CHECK_THROWS_AS(source.start([](const CaptureBlock&) {}), std::runtime_error);
    CHECK_FALSE(source.is_active());
}

TEST_CASE("the loopback source waits for its first block and follows the default endpoint",
          "[test_meeting_sd_stream]") {
    Rig rig;
    const fv::audio::LoopbackEndpoint speakers{
        .id = "{speakers}", .name = "Speakers", .is_default = true, .rate = 48000, .channels = 2};
    rig.backend->set_loopbacks({speakers});
    std::vector<std::int16_t> mono;
    fv::meeting::LoopbackSource source(rig.backend, rig.clock, speakers, 2.0);
    std::thread device([&] {
        // The endpoint delivers its first block while start() waits.
        while (rig.backend->streams().empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const std::vector<std::int16_t> stereo = {100, 300, -100, -300};
        rig.stream()->deliver(stereo);
    });
    source.start(
        [&](const CaptureBlock& block) { mono.assign(block.frames, block.frames + block.count); });
    device.join();
    CHECK(source.is_active());
    CHECK(mono == std::vector<std::int16_t>{200, -200});
    CHECK(source.is_default_device_current() == true);
    rig.backend->set_loopbacks({{.id = "{hdmi}", .name = "HDMI", .is_default = true}, speakers});
    CHECK(source.is_default_device_current() == false);
    rig.stream()->lose("invalidated");
    CHECK_FALSE(source.is_active());
    source.stop();
}

TEST_CASE("stereo content on a surround loopback keeps its level", "[test_meeting_sd_stream]") {
    Rig rig;
    const fv::audio::LoopbackEndpoint surround{.id = "{surround}",
                                               .name = "Speakers 5.1",
                                               .is_default = true,
                                               .rate = 48000,
                                               .channels = 6};
    rig.backend->set_loopbacks({surround});
    std::vector<std::int16_t> mono;
    fv::meeting::LoopbackSource source(rig.backend, rig.clock, surround, 2.0);
    std::thread device([&] {
        while (rig.backend->streams().empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Stereo content on a 5.1 endpoint: the centre and surrounds are silent, and the LFE
        // (the fourth channel) is not mixed in.
        const std::vector<std::int16_t> frames = {900, 300, 0, 0, 0, 0, -901, -300, 0, 7, 0, 0};
        rig.stream()->deliver(frames);
    });
    source.start(
        [&](const CaptureBlock& block) { mono.assign(block.frames, block.frames + block.count); });
    device.join();
    CHECK(mono == std::vector<std::int16_t>{600, -600});
    source.stop();
}

TEST_CASE("a loopback that never delivers is inactive after the wait", "[test_meeting_sd_stream]") {
    Rig rig;
    const fv::audio::LoopbackEndpoint speakers{
        .id = "{speakers}", .name = "Speakers", .is_default = true};
    rig.backend->set_loopbacks({speakers});
    fv::meeting::LoopbackSource source(rig.backend, rig.clock, speakers, 0.05);
    source.start([](const CaptureBlock&) {});
    CHECK_FALSE(source.is_active());
    source.stop();
}

TEST_CASE("a surround loopback mixes its centre and surrounds in at -3 dB", "[sources]") {
    using fv::meeting::loopback_mix_weights;
    const auto mono = [](int channels, std::vector<std::int16_t> frame) {
        Rig rig;
        const fv::audio::LoopbackEndpoint surround{.id = "{surround}",
                                                   .name = "Speakers",
                                                   .is_default = true,
                                                   .rate = 48000,
                                                   .channels = channels};
        rig.backend->set_loopbacks({surround});
        std::vector<std::int16_t> out;
        fv::meeting::LoopbackSource source(rig.backend, rig.clock, surround, 2.0);
        std::thread device([&] {
            while (rig.backend->streams().empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            rig.stream()->deliver(frame);
        });
        source.start([&](const CaptureBlock& block) {
            out.assign(block.frames, block.frames + block.count);
        });
        device.join();
        source.stop();
        REQUIRE(out.size() == 1);
        return out[0];
    };
    // Centre-only speech on 5.1 and 7.1.
    CHECK(mono(6, {0, 0, 1000, 0, 0, 0}) == 707);
    CHECK(mono(8, {0, 0, 1000, 0, 0, 0, 0, 0}) == 707);
    // A surround pair at the same level as a front pair's mean reaches -3 dB of it.
    CHECK(mono(4, {0, 0, 1000, 1000}) == 707);
    CHECK(mono(8, {0, 0, 0, 0, 0, 0, 1000, 1000}) == 707);
    CHECK(mono(6, {0, 0, 0, 30000, 0, 0}) == 0); // the LFE
    // Saturates rather than wraps.
    CHECK(mono(6, {32767, 32767, 32767, 0, 32767, 32767}) == 32767);
    CHECK(mono(6, {-32768, -32768, -32768, 0, -32768, -32768}) == -32768);
    // No known order: the front pair alone.
    CHECK(loopback_mix_weights(2).empty());
    CHECK(loopback_mix_weights(3).empty());
    CHECK(mono(3, {100, 300, 5000}) == 200);
}
