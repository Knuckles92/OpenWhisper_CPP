// System audio through WASAPI loopback: the conversion of the mix format to interleaved int16,
// the hand-over in blocks, the FakeBackend's loopback scripting, and the real endpoints. Listing
// endpoints opens no stream ([platform]); capturing one is [interactive] (amendment 14).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "audio/backend.h"
#include "audio/detail/loopback_pump.h"
#include "audio/testing/fake_backend.h"
#include "testkit/testkit.h"

namespace {

using namespace std::chrono_literals;
using fv::audio::BlockStatus;
using fv::audio::LoopbackEndpoint;
using fv::audio::detail::float_to_int16;
using fv::audio::detail::LoopbackPump;
using fv::audio::detail::LoopbackSampleFormat;
using fv::audio::detail::MixFormat;
using fv::audio::testing::FakeBackend;
using fv::audio::testing::FakeStream;

// A CaptureFn's view of what arrived: preallocated, so the pump can run under RealtimeGuard.
struct Blocks {
    static constexpr std::size_t kCapacity = 1 << 16;
    std::vector<std::int16_t> samples = std::vector<std::int16_t>(kCapacity);
    std::size_t used = 0;
    std::vector<std::size_t> frames = std::vector<std::size_t>(64);
    std::vector<BlockStatus> status = std::vector<BlockStatus>(64);
    std::size_t blocks = 0;
    std::size_t channels = 1;

    std::span<const std::int16_t> all() const { return std::span(samples).first(used); }
};

void collect(void* user, const std::int16_t* frames, std::size_t count, BlockStatus status) {
    auto* blocks = static_cast<Blocks*>(user);
    const std::size_t samples = count * blocks->channels;
    for (std::size_t i = 0; i < samples && blocks->used < Blocks::kCapacity; ++i) {
        blocks->samples[blocks->used++] = frames[i];
    }
    if (blocks->blocks < blocks->frames.size()) {
        blocks->frames[blocks->blocks] = count;
        blocks->status[blocks->blocks] = status;
    }
    ++blocks->blocks;
}

std::vector<std::size_t> block_sizes(const Blocks& blocks) {
    return {blocks.frames.begin(),
            blocks.frames.begin() + static_cast<std::ptrdiff_t>(blocks.blocks)};
}

LoopbackEndpoint speakers() {
    return {.id = "{0.0.0.00000000}.{speakers}",
            .name = "Speakers (Realtek(R) Audio)",
            .is_default = true,
            .rate = 48000,
            .channels = 2};
}

LoopbackEndpoint headset() {
    return {.id = "{0.0.0.00000000}.{headset}",
            .name = "Headset Earphone (USB Audio)",
            .is_default = false,
            .rate = 44100,
            .channels = 6};
}

} // namespace

TEST_CASE("float samples become int16 as numpy's clip and astype do") {
    // np.clip(np.float32(x) * 32767.0, -32768.0, 32767.0).astype(np.int16), worked by hand in
    // float32: the product is rounded to float32, then truncated toward zero.
    CHECK(float_to_int16(0.0F) == 0);
    CHECK(float_to_int16(-0.0F) == 0);
    CHECK(float_to_int16(0.5F) == 16383);   // 16383.5
    CHECK(float_to_int16(-0.5F) == -16383); // -16383.5 truncates toward zero
    CHECK(float_to_int16(0.25F) == 8191);   // 8191.75
    CHECK(float_to_int16(-0.25F) == -8191);
    CHECK(float_to_int16(0.1F) == 3276);    // 3276.7000732421875
    CHECK(float_to_int16(0.3F) == 9830);    // 9830.1005859375
    CHECK(float_to_int16(-0.7F) == -22936); // -22936.900390625
    CHECK(float_to_int16(0.99999F) == 32766);
    CHECK(float_to_int16(1e-5F) == 0);
    CHECK(float_to_int16(-1.0F / 32768.0F) == 0); // -0.999969482421875
    CHECK(float_to_int16(1.0F) == 32767);
    CHECK(float_to_int16(-1.0F) == -32767); // full scale is 32767, not 32768
    // 1 + 2^-15 scales to 32767.99997, which float32 rounds to 32768.0 before the clip.
    CHECK(float_to_int16(1.000030517578125F) == 32767);
    CHECK(float_to_int16(-1.000030517578125F) == -32768);
    CHECK(float_to_int16(1.5F) == 32767);
    CHECK(float_to_int16(-1.5F) == -32768);
    CHECK(float_to_int16(std::numeric_limits<float>::infinity()) == 32767);
    CHECK(float_to_int16(-std::numeric_limits<float>::infinity()) == -32768);
    CHECK(float_to_int16(std::numeric_limits<float>::quiet_NaN()) == 0);
}

TEST_CASE("a stereo float packet reaches the capture function in blocks of 1024 frames") {
    Blocks blocks;
    blocks.channels = 2;
    LoopbackPump pump(48000, 2, LoopbackSampleFormat::Float32, &collect, &blocks);
    std::vector<float> packet(2500 * 2);
    for (std::size_t i = 0; i < packet.size(); ++i) {
        // -1.5625 to 1.53, so some samples clip.
        packet[i] = static_cast<float>(static_cast<int>(i % 200) - 100) / 64.0F;
    }
    {
        const fv::testkit::RealtimeGuard guard;
        pump.packet(packet.data(), 2500, false, false, 100.0, 100.5);
    }
    CHECK(block_sizes(blocks) == std::vector<std::size_t>{1024, 1024, 452});
    REQUIRE(blocks.used == packet.size());
    for (std::size_t i = 0; i < packet.size(); ++i) {
        REQUIRE(blocks.samples[i] == float_to_int16(packet[i]));
    }
    // Left and right stay interleaved: sample 1 is the first frame's right channel.
    CHECK(blocks.samples[0] == float_to_int16(-100.0F / 64.0F));
    CHECK(blocks.samples[0] == -32768);
    CHECK(blocks.samples[1] == float_to_int16(-99.0F / 64.0F));
    // Each block is stamped where its first frame lies in the packet.
    CHECK(blocks.status[0].adc_time == 100.0);
    CHECK(blocks.status[1].adc_time == 100.0 + 1024.0 / 48000.0);
    CHECK(blocks.status[2].adc_time == 100.0 + 2048.0 / 48000.0);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(blocks.status[i].current_time == 100.5);
        CHECK_FALSE(blocks.status[i].overflow);
        CHECK_FALSE(blocks.status[i].underflow);
    }
}

TEST_CASE("a 6-channel mix is delivered with every channel interleaved") {
    Blocks blocks;
    blocks.channels = 6;
    LoopbackPump pump(44100, 6, LoopbackSampleFormat::Float32, &collect, &blocks);
    // Two frames: front left, front right, centre, LFE, back left, back right.
    const std::vector<float> packet{0.5F, -0.5F, 0.25F, -0.25F, 1.0F,  -1.0F,
                                    0.1F, 0.3F,  -0.7F, 2.0F,   -2.0F, 0.0F};
    pump.packet(packet.data(), 2, false, false, 7.0, 7.25);
    CHECK(block_sizes(blocks) == std::vector<std::size_t>{2});
    CHECK(std::vector<std::int16_t>(blocks.all().begin(), blocks.all().end()) ==
          std::vector<std::int16_t>{16383, -16383, 8191, -8191, 32767, -32767, 3276, 9830, -22936,
                                    32767, -32768, 0});
}

TEST_CASE("a 16-bit PCM mix passes through unchanged") {
    Blocks blocks;
    blocks.channels = 2;
    LoopbackPump pump(48000, 2, LoopbackSampleFormat::Int16, &collect, &blocks);
    std::vector<std::int16_t> packet(1500 * 2);
    for (std::size_t i = 0; i < packet.size(); ++i) {
        packet[i] = static_cast<std::int16_t>(static_cast<int>((i * 37) % 65536) - 32768);
    }
    pump.packet(packet.data(), 1500, false, false, 1.0, 2.0);
    CHECK(block_sizes(blocks) == std::vector<std::size_t>{1024, 476});
    CHECK(std::vector<std::int16_t>(blocks.all().begin(), blocks.all().end()) == packet);
}

TEST_CASE("silent packets and quiet stretches become zeros") {
    Blocks blocks;
    blocks.channels = 2;
    LoopbackPump pump(48000, 2, LoopbackSampleFormat::Float32, &collect, &blocks);
    // AUDCLNT_BUFFERFLAGS_SILENT: whatever the buffer holds is ignored.
    const std::vector<float> loud(300 * 2, 0.75F);
    pump.packet(loud.data(), 300, true, false, 5.0, 5.1);
    CHECK(block_sizes(blocks) == std::vector<std::size_t>{300});
    CHECK(std::ranges::all_of(blocks.all(), [](std::int16_t s) { return s == 0; }));

    // An endpoint that delivers nothing for 100 ms: that long of zeros, stamped from where the
    // quiet began, as soundcard's reads return zeros for the time that passed.
    blocks = Blocks{};
    blocks.channels = 2;
    const std::size_t quiet = pump.frames_for(0.1);
    CHECK(quiet == 4800);
    CHECK(pump.frames_for(0.0) == 0);
    CHECK(pump.frames_for(-1.0) == 0);
    CHECK(pump.frames_for(1.0 / 48000.0 * 2.5) == 2);
    {
        const fv::testkit::RealtimeGuard guard;
        pump.silence(quiet, 10.0, 10.1);
    }
    CHECK(block_sizes(blocks) == std::vector<std::size_t>{1024, 1024, 1024, 1024, 704});
    CHECK(blocks.used == 4800 * 2);
    CHECK(std::ranges::all_of(blocks.all(), [](std::int16_t s) { return s == 0; }));
    CHECK(blocks.status[4].adc_time == 10.0 + 4096.0 / 48000.0);
    CHECK(blocks.status[4].current_time == 10.1);
}

TEST_CASE("a data discontinuity marks the first block of its packet as an overflow") {
    Blocks blocks;
    LoopbackPump pump(48000, 1, LoopbackSampleFormat::Float32, &collect, &blocks);
    const std::vector<float> packet(2048, 0.0F);
    pump.packet(packet.data(), 2048, false, true, std::numeric_limits<double>::quiet_NaN(), 3.0);
    REQUIRE(blocks.blocks == 2);
    CHECK(blocks.status[0].overflow);
    CHECK_FALSE(blocks.status[1].overflow);
    // A packet without a usable timestamp leaves every block's capture time unknown.
    CHECK(std::isnan(blocks.status[0].adc_time));
    CHECK(std::isnan(blocks.status[1].adc_time));
    CHECK(blocks.status[1].current_time == 3.0);
}

TEST_CASE("packets read from the capture client are stamped on the QPC clock") {
    Blocks blocks;
    LoopbackPump pump(48000, 1, LoopbackSampleFormat::Float32, &collect, &blocks);
    const std::vector<float> packet(480, 0.5F);
    {
        const fv::testkit::RealtimeGuard guard;
        pump.start(100.0);
        // WASAPI flags the first packet of every stream as a discontinuity: not a loss.
        pump.read({.data = packet.data(),
                   .frames = 480,
                   .discontinuity = true,
                   .qpc_position = 1'000'050'000},
                  100.02);
        // A later discontinuity is.
        pump.read({.data = packet.data(),
                   .frames = 480,
                   .discontinuity = true,
                   .qpc_position = 1'000'150'000},
                  100.03);
        // TIMESTAMP_ERROR, or no position at all, leaves the capture time unknown.
        pump.read({.data = packet.data(),
                   .frames = 480,
                   .timestamp_error = true,
                   .qpc_position = 1'000'250'000},
                  100.04);
        pump.read({.data = packet.data(), .frames = 480}, 100.05);
        pump.read({.data = packet.data(), .frames = 480, .silent = true, .qpc_position = 1},
                  100.06);
    }
    REQUIRE(blocks.blocks == 5);
    CHECK_FALSE(blocks.status[0].overflow);
    CHECK(blocks.status[1].overflow);
    CHECK_FALSE(blocks.status[2].overflow);
    // pu64QPCPosition is in 100 ns units: 1'000'050'000 is 100.005 s.
    CHECK(blocks.status[0].adc_time == 1'000'050'000 * 1e-7);
    CHECK(blocks.status[0].current_time == 100.02);
    CHECK(blocks.status[1].adc_time == 1'000'150'000 * 1e-7);
    CHECK(std::isnan(blocks.status[2].adc_time));
    CHECK(std::isnan(blocks.status[3].adc_time));
    CHECK(blocks.status[3].current_time == 100.05);
    CHECK(blocks.status[4].adc_time == 1e-7);
    CHECK(std::ranges::all_of(blocks.all().first(4 * 480),
                              [](std::int16_t s) { return s == 16383; }));
    CHECK(
        std::ranges::all_of(blocks.all().subspan(4 * 480), [](std::int16_t s) { return s == 0; }));

    // A new stream's first packet is again not a loss.
    pump.start(200.0);
    pump.read({.data = packet.data(), .frames = 480, .discontinuity = true}, 200.01);
    REQUIRE(blocks.blocks == 6);
    CHECK_FALSE(blocks.status[5].overflow);
}

TEST_CASE("a quiet endpoint yields zeros for the time that passed, from the last packet on") {
    // Times are binary fractions of a second, so that the 100 ms threshold is compared exactly.
    const auto frames_in = [](const Blocks& blocks, std::size_t from) {
        std::size_t total = 0;
        for (std::size_t i = from; i < blocks.blocks; ++i) {
            total += blocks.frames[i];
        }
        return total;
    };
    Blocks blocks;
    blocks.channels = 2;
    LoopbackPump pump(48000, 2, LoopbackSampleFormat::Float32, &collect, &blocks);
    std::size_t early = 0;
    {
        const fv::testkit::RealtimeGuard guard;
        pump.start(8.0);
        // Under 100 ms of quiet is a late packet, not silence.
        pump.idle(8.0625);
        early = blocks.blocks;
        // 125 ms after the start: 6000 frames of zeros stamped from the start.
        pump.idle(8.125);
    }
    CHECK(early == 0);
    CHECK(block_sizes(blocks) == std::vector<std::size_t>{1024, 1024, 1024, 1024, 1024, 880});
    CHECK(blocks.status[0].adc_time == 8.0);
    CHECK(blocks.status[0].current_time == 8.125);
    CHECK(blocks.status[5].adc_time == 8.0 + 5120.0 / 48000.0);
    CHECK(blocks.used == 6000 * 2);
    CHECK(std::ranges::all_of(blocks.all(), [](std::int16_t s) { return s == 0; }));

    // The next quiet starts where those zeros end; a late wake hands over all of it at once.
    blocks = Blocks{};
    blocks.channels = 2;
    pump.idle(8.1875); // 62.5 ms after the last zeros: nothing yet
    CHECK(blocks.blocks == 0);
    pump.idle(8.375);
    CHECK(frames_in(blocks, 0) == 12000);
    CHECK(blocks.status[0].adc_time == 8.125);

    // A packet ends the quiet: the next zeros count from its hand-over time.
    blocks = Blocks{};
    blocks.channels = 2;
    const std::vector<float> packet(480 * 2, 0.0F);
    pump.read({.data = packet.data(), .frames = 480, .qpc_position = 84'000'000}, 8.4375);
    pump.idle(8.5);
    CHECK(blocks.blocks == 1);
    pump.idle(8.5625);
    CHECK(frames_in(blocks, 1) == 6000);
    REQUIRE(blocks.blocks > 1);
    CHECK(blocks.status[1].adc_time == 8.4375);
    CHECK(blocks.status[1].current_time == 8.5625);

    // Whole frames only, and what is left over is delivered later, so nothing drifts: at 8 Hz,
    // 0.4375 s of quiet is 3 frames, and the half frame left joins the next wake's.
    Blocks slow;
    LoopbackPump coarse(8, 1, LoopbackSampleFormat::Float32, &collect, &slow);
    coarse.start(0.0);
    coarse.idle(0.4375);
    CHECK(frames_in(slow, 0) == 3);
    coarse.idle(0.5);
    REQUIRE(slow.blocks == 2);
    CHECK(slow.frames[1] == 1);
    CHECK(slow.status[1].adc_time == 0.375);
}

TEST_CASE("the stream takes the mix format when it can and asks the engine to convert otherwise") {
    using fv::audio::detail::choose_loopback_format;
    using Kind = MixFormat::Kind;
    const MixFormat float_mix{
        .kind = Kind::Float, .bits_per_sample = 32, .valid_bits = 32, .rate = 48000, .channels = 2};
    auto choice = choose_loopback_format(float_mix, 48000, 2);
    CHECK(choice.format == LoopbackSampleFormat::Float32);
    CHECK_FALSE(choice.convert);

    choice = choose_loopback_format(
        {.kind = Kind::Pcm, .bits_per_sample = 16, .valid_bits = 16, .rate = 44100, .channels = 6},
        44100, 6);
    CHECK(choice.format == LoopbackSampleFormat::Int16);
    CHECK_FALSE(choice.convert);

    // 24-bit PCM, or a mix that changed after the endpoint was listed: float32 at the listed
    // rate and channels, converted by the audio engine.
    for (const MixFormat& other : {MixFormat{.kind = Kind::Pcm,
                                             .bits_per_sample = 24,
                                             .valid_bits = 24,
                                             .rate = 48000,
                                             .channels = 2},
                                   MixFormat{.kind = Kind::Pcm,
                                             .bits_per_sample = 32,
                                             .valid_bits = 24,
                                             .rate = 48000,
                                             .channels = 2},
                                   MixFormat{.kind = Kind::Other,
                                             .bits_per_sample = 32,
                                             .valid_bits = 32,
                                             .rate = 48000,
                                             .channels = 2},
                                   MixFormat{.kind = Kind::Float,
                                             .bits_per_sample = 32,
                                             .valid_bits = 32,
                                             .rate = 96000,
                                             .channels = 2},
                                   MixFormat{.kind = Kind::Float,
                                             .bits_per_sample = 32,
                                             .valid_bits = 32,
                                             .rate = 48000,
                                             .channels = 8}}) {
        choice = choose_loopback_format(other, 48000, 2);
        CHECK(choice.format == LoopbackSampleFormat::Float32);
        CHECK(choice.convert);
    }
}

TEST_CASE("the fake backend lists and opens scripted render endpoints") {
    const auto backend = std::make_shared<FakeBackend>();
    CHECK(backend->loopbacks().empty());
    backend->set_loopbacks({speakers(), headset()});
    const auto listed = backend->loopbacks();
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].is_default);
    CHECK(listed[1].rate == 44100);
    CHECK(listed[1].channels == 6);

    Blocks blocks;
    blocks.channels = 6;
    std::vector<std::string> lost;
    auto stream = backend->open_loopback(headset(), &collect, &blocks,
                                         [&](std::string reason) { lost.push_back(reason); });
    REQUIRE(stream);
    CHECK((*stream)->active());
    const auto fake = backend->streams().front();
    CHECK(fake->kind() == FakeStream::Kind::Loopback);
    CHECK(fake->endpoint().id == headset().id);
    CHECK(fake->config().rate == 44100);
    CHECK(fake->config().channels == 6);
    CHECK(fake->config().frames_per_block == 1024);
    CHECK(backend->open_streams() == 1);

    // Loopback is not a PortAudio stream, so it does not hold a refresh off.
    CHECK(backend->refresh());

    const std::vector<std::int16_t> one(1024 * 6, 7);
    fake->deliver(one, BlockStatus{.adc_time = 1.0, .current_time = 1.5});
    CHECK(block_sizes(blocks) == std::vector<std::size_t>{1024});
    CHECK(blocks.status[0].adc_time == 1.0);
    const std::vector<std::int16_t> too_big(1025 * 6, 7);
    CHECK_THROWS_AS(fake->deliver(too_big), std::logic_error);
    const std::vector<std::int16_t> pcm(2500 * 6, 3);
    CHECK(fake->deliver_blocks(pcm) == 2048);
    CHECK(block_sizes(blocks) == std::vector<std::size_t>{1024, 1024, 1024});

    fake->lose("Loopback capture of 'Headset Earphone (USB Audio)' ended: The audio endpoint was "
               "removed or disabled (AUDCLNT_E_DEVICE_INVALIDATED) (0x88890004)");
    REQUIRE(lost.size() == 1);
    CHECK_THAT(lost[0], Catch::Matchers::ContainsSubstring("AUDCLNT_E_DEVICE_INVALIDATED"));
    CHECK_FALSE((*stream)->active());
    stream->reset();
    CHECK(fake->closed());
    CHECK(backend->open_streams() == 0);
}

TEST_CASE("an aborted or closed loopback stream never reports the endpoint lost") {
    const auto backend = std::make_shared<FakeBackend>();
    backend->set_loopbacks({speakers()});
    Blocks blocks;
    int lost = 0;
    auto stream =
        backend->open_loopback(speakers(), &collect, &blocks, [&](const std::string&) { ++lost; });
    REQUIRE(stream);
    const auto fake = backend->streams().front();
    (*stream)->abort();
    CHECK_FALSE((*stream)->active());
    fake->lose();
    stream->reset();
    fake->lose();
    CHECK(lost == 0);
}

TEST_CASE("loopback opens and listings fail as scripted") {
    const auto backend = std::make_shared<FakeBackend>();
    backend->set_loopbacks({speakers(), headset()});
    backend->fail_loopback_open(headset().id,
                                "Error opening loopback endpoint 'Headset Earphone (USB Audio)': "
                                "The audio endpoint is in use (AUDCLNT_E_DEVICE_IN_USE) "
                                "(0x8889000A)");
    const auto refused = backend->open_loopback(headset(), &collect, nullptr, {});
    REQUIRE_FALSE(refused);
    CHECK_THAT(refused.error(), Catch::Matchers::ContainsSubstring("AUDCLNT_E_DEVICE_IN_USE"));

    // An endpoint unplugged since it was listed.
    const auto gone = backend->open_loopback({.id = "{0.0.0.00000000}.{gone}", .name = "Gone"},
                                             &collect, nullptr, {});
    REQUIRE_FALSE(gone);
    CHECK(gone.error() ==
          "Error querying loopback endpoint '{0.0.0.00000000}.{gone}': Element not found "
          "(0x80070490)");
    CHECK(backend->streams().empty());
    CHECK(backend->open_loopback(speakers(), &collect, nullptr, {}));

    backend->fail_loopback_listing("Error listing loopback endpoints: The Windows audio service "
                                   "is not running (AUDCLNT_E_SERVICE_NOT_RUNNING) (0x88890010)");
    CHECK_THROWS_AS(backend->loopbacks(), std::runtime_error);
    backend->fail_loopback_listing(std::nullopt);
    CHECK(backend->loopbacks().size() == 2);
}

TEST_CASE("the system's render endpoints are listed without capturing", "[platform]") {
    const auto backend = fv::audio::make_portaudio_backend();
    std::vector<LoopbackEndpoint> endpoints;
    REQUIRE_NOTHROW(endpoints = backend->loopbacks());
    std::cout << "Render endpoints:\n";
    int defaults = 0;
    std::optional<std::string> flagged;
    for (const auto& endpoint : endpoints) {
        std::cout << "  " << endpoint.name << " | " << endpoint.rate << " Hz " << endpoint.channels
                  << " ch" << (endpoint.is_default ? " | default" : "") << '\n';
        CHECK_FALSE(endpoint.id.empty());
        CHECK(endpoint.rate > 0);
        CHECK(endpoint.channels > 0);
        defaults += endpoint.is_default ? 1 : 0;
        if (endpoint.is_default) {
            flagged = endpoint.id;
        }
    }
    CHECK(defaults <= 1);
    // The watchdog's cheap query names the endpoint the listing flags.
    CHECK(backend->default_loopback_id() == flagged);
#if !defined(_WIN32)
    CHECK(endpoints.empty());
    CHECK_FALSE(backend->open_loopback({.id = "any"}, &collect, nullptr, {}));
#endif
}

namespace {

// A 440 Hz tone at a third of full scale for as long as the stream pulls.
class Tone final : public fv::audio::PlaybackSource {
public:
    std::size_t read(std::span<std::int16_t> out) override {
        for (std::int16_t& sample : out) {
            const double phase =
                2.0 * std::numbers::pi * 440.0 * static_cast<double>(n_++) / 44100.0;
            sample = static_cast<std::int16_t>(std::sin(phase) * 10000.0);
        }
        return out.size();
    }

private:
    std::uint64_t n_ = 0;
};

struct Meter {
    std::atomic<std::size_t> frames{0};
    std::atomic<std::size_t> blocks{0};
    std::atomic<std::size_t> largest{0};
    std::atomic<double> sum_squares{0.0};
    std::atomic<bool> stamped{false};   // a usable capture time
    std::atomic<bool> unstamped{false}; // a block without a current time at all
    std::size_t channels = 2;
};

void meter(void* user, const std::int16_t* frames, std::size_t count, BlockStatus status) {
    auto* m = static_cast<Meter*>(user);
    double sum = 0.0;
    for (std::size_t i = 0; i < count * m->channels; ++i) {
        sum += static_cast<double>(frames[i]) * frames[i];
    }
    m->sum_squares.store(m->sum_squares.load() + sum);
    m->frames += count;
    ++m->blocks;
    if (count > m->largest) {
        m->largest = count;
    }
    if (std::isfinite(status.adc_time) && std::isfinite(status.current_time) &&
        status.adc_time <= status.current_time) {
        m->stamped = true;
    }
    if (std::isnan(status.current_time)) {
        m->unstamped = true;
    }
}

} // namespace

TEST_CASE("two seconds of a tone played to the default endpoint come back through loopback",
          "[interactive]") {
    const auto backend = fv::audio::make_portaudio_backend();
    const auto endpoints = backend->loopbacks();
    const auto chosen = std::ranges::find_if(endpoints, &LoopbackEndpoint::is_default);
    REQUIRE(chosen != endpoints.end());
    Meter meter_state;
    meter_state.channels = static_cast<std::size_t>(chosen->channels);
    std::atomic<int> lost{0};
    auto stream =
        backend->open_loopback(*chosen, &meter, &meter_state, [&](const std::string&) { ++lost; });
    REQUIRE(stream);
    Tone tone;
    auto playback = backend->open_playback({.device_index = -1, .rate = 44100}, tone);
    REQUIRE(playback);
    std::this_thread::sleep_for(2s);
    (*playback)->abort();
    playback->reset();
    CHECK((*stream)->active());
    (*stream)->abort();
    CHECK_FALSE((*stream)->active());
    const std::size_t frames = meter_state.frames;
    stream->reset();
    CHECK(lost == 0);
    // Two seconds of the mix rate, less what the open and the first packet take.
    CHECK(frames > static_cast<std::size_t>(chosen->rate * 3 / 2));
    CHECK(meter_state.largest <= 1024);
    CHECK(meter_state.stamped);
    const double rms = std::sqrt(meter_state.sum_squares.load() /
                                 static_cast<double>(frames * meter_state.channels));
    std::cout << "Loopback of '" << chosen->name << "': " << frames << " frames in "
              << meter_state.blocks << " blocks, RMS " << rms << '\n';
    CHECK(rms > 300.0);
}

TEST_CASE("a quiet endpoint still delivers blocks, and a default change leaves the stream on its "
          "endpoint",
          "[interactive]") {
    const auto backend = fv::audio::make_portaudio_backend();
    const auto endpoints = backend->loopbacks();
    const auto chosen = std::ranges::find_if(endpoints, &LoopbackEndpoint::is_default);
    REQUIRE(chosen != endpoints.end());
    Meter meter_state;
    meter_state.channels = static_cast<std::size_t>(chosen->channels);
    std::atomic<int> lost{0};
    auto stream =
        backend->open_loopback(*chosen, &meter, &meter_state, [&](const std::string&) { ++lost; });
    REQUIRE(stream);
    // Nothing plays: blocks keep coming (silent packets or synthesized zeros every 100 ms).
    std::this_thread::sleep_for(1s);
    CHECK(meter_state.frames > static_cast<std::size_t>(chosen->rate / 2));

    std::cout << "Change the default playback device away from '" << chosen->name
              << "', or unplug it, within 20 seconds.\n";
    bool moved = false;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!moved && lost == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(250ms);
        for (const auto& endpoint : backend->loopbacks()) {
            moved = moved || (endpoint.is_default && endpoint.id != chosen->id);
        }
        moved = moved || backend->loopbacks().empty();
    }
    REQUIRE((moved || lost == 1));
    if (lost == 0) {
        // WASAPI keeps capturing the endpoint it opened; following the default is the meeting
        // watchdog's job.
        const std::size_t before = meter_state.frames;
        std::this_thread::sleep_for(500ms);
        CHECK((*stream)->active());
        CHECK(meter_state.frames > before);
    } else {
        CHECK_FALSE((*stream)->active());
    }
    stream->reset();
    CHECK(lost <= 1);
}

TEST_CASE("microphone blocks carry PortAudio's ADC and current times", "[interactive]") {
    const auto backend = fv::audio::make_portaudio_backend();
    Meter meter_state;
    meter_state.channels = 1;
    auto stream = backend->open_capture({.device_index = -1}, &meter, &meter_state, {});
    REQUIRE(stream);
    std::this_thread::sleep_for(1s);
    (*stream)->abort();
    stream->reset();
    CHECK(meter_state.frames > 30000);
    // PortAudio hands every callback its time info; MME may report a current time of 0, which
    // the meeting capture then ignores, so only the presence of the times is checked.
    CHECK_FALSE(meter_state.unstamped);
    std::cout << "PortAudio capture stamped: " << (meter_state.stamped ? "yes" : "no") << '\n';
}
