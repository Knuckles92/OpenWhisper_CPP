// audio::testing::FakeBackend, the seam the recorder's tests (M1.10) port against.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "audio/detail/portaudio_error.h"
#include "audio/testing/fake_backend.h"
#include "audio_test_support.h"
#include "testkit/testkit.h"

namespace {

using fv::audio::BlockStatus;
using fv::audio::test::fixture;
using fv::audio::testing::FakeBackend;

// A CaptureFn as the recorder writes one: preallocated storage and counters only.
struct Sink {
    std::int16_t samples[4096] = {};
    std::size_t frames = 0;
    int overflows = 0;
};

void capture(void* user, const std::int16_t* frames, std::size_t count, BlockStatus status) {
    auto* sink = static_cast<Sink*>(user);
    for (std::size_t i = 0; i < count && sink->frames + i < 4096; ++i) {
        sink->samples[sink->frames + i] = frames[i];
    }
    sink->frames += count;
    sink->overflows += status.overflow ? 1 : 0;
}

} // namespace

TEST_CASE("scripted capture blocks reach the capture function without allocating") {
    const auto backend = fv::audio::test::windows_backend();
    Sink sink;
    std::vector<std::string> lost;
    auto stream = backend->open_capture({.device_index = 1}, &capture, &sink,
                                        [&](std::string reason) { lost.push_back(reason); });
    REQUIRE(stream);
    const auto handle = backend->streams().front();
    const auto pcm = fv::audio::testing::read_pcm(fixture("tone-44k1-mono.wav"));
    {
        const fv::testkit::RealtimeGuard guard;
        for (std::size_t at = 0; at + 1024 <= 4096; at += 1024) {
            handle->deliver(std::span(pcm).subspan(at, 1024), BlockStatus{.overflow = at == 0});
        }
    }
    CHECK(sink.frames == 4096);
    CHECK(sink.overflows == 1);
    CHECK(std::equal(pcm.begin(), pcm.begin() + 4096, sink.samples));
    CHECK((*stream)->active());

    handle->lose("unplugged");
    CHECK(lost == std::vector<std::string>{"unplugged"});
}

TEST_CASE("a wav is delivered in whole blocks of the stream's size") {
    const auto backend = fv::audio::test::windows_backend();
    Sink sink;
    fv::audio::StreamConfig config{.device_index = 1};
    config.frames_per_block = 1000;
    auto stream = backend->open_capture(config, &capture, &sink, {});
    REQUIRE(stream);
    const auto pcm = fv::audio::testing::read_pcm(fixture("tone-44k1-mono.wav"));
    REQUIRE(pcm.size() == 35280); // 0.8 s at 44.1 kHz
    const auto handle = backend->streams().front();
    CHECK(handle->deliver_blocks(std::span(pcm).first(4500)) == 4000);
    CHECK(sink.frames == 4000);
    CHECK(std::equal(pcm.begin(), pcm.begin() + 4000, sink.samples));
}

TEST_CASE("an aborted or closed capture never reports the device lost") {
    const auto backend = fv::audio::test::windows_backend();
    Sink sink;
    int lost = 0;
    auto stream = backend->open_capture({.device_index = 1}, &capture, &sink,
                                        [&](const std::string&) { ++lost; });
    REQUIRE(stream);
    const auto handle = backend->streams().front();
    (*stream)->abort();
    handle->lose();
    stream->reset();
    CHECK(handle->closed());
    handle->lose();
    CHECK(lost == 0);
    CHECK(backend->open_streams() == 0);
}

TEST_CASE("scripted open failures and an unplugged microphone") {
    const auto backend = fv::audio::test::windows_backend();
    backend->fail_open(2, "Error opening InputStream: Device unavailable [PaErrorCode -9985]");
    const auto refused = backend->open_capture({.device_index = 2}, &capture, nullptr, {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().find("Device unavailable") != std::string::npos);
    CHECK(backend->streams().empty());

    backend->keep_devices(2);
    CHECK(backend->inputs().size() == 2);
    CHECK(backend->check_input({.device_index = 2}).error() == "Error querying device 2");
    const auto unplugged = backend->open_capture({.device_index = 5}, &capture, nullptr, {});
    REQUIRE_FALSE(unplugged);
    CHECK(unplugged.error() == "Error querying device 5");
    CHECK(backend->streams().empty());
    CHECK_FALSE(backend->open_loopback({}, &capture, nullptr, {}));
}

TEST_CASE("a refresh that cannot restart the audio system throws instead of reading as busy") {
    const auto backend = fv::audio::test::windows_backend();
    backend->fail_refresh("Error initializing PortAudio: Host error [PaErrorCode -9999]");
    CHECK_THROWS_WITH(backend->refresh(),
                      "Error initializing PortAudio: Host error [PaErrorCode -9999]");
    backend->fail_refresh(std::nullopt);
    CHECK(backend->refresh());
    CHECK(backend->lifecycle() == std::vector<std::string>{"terminate", "terminate", "initialize"});
}

TEST_CASE("errors read as sounddevice's PortAudioError") {
    using fv::audio::detail::portaudio_error_text;
    CHECK(portaudio_error_text("Error opening InputStream", "Invalid sample rate", -9997) ==
          "Error opening InputStream: Invalid sample rate [PaErrorCode -9997]");
    CHECK(portaudio_error_text("", "Device unavailable", -9985) ==
          "Device unavailable [PaErrorCode -9985]");
    CHECK(portaudio_error_text("Error starting stream", "Unanticipated host error", -9999,
                               fv::audio::detail::PortAudioHostError{"Windows WASAPI", -2004287478,
                                                                     "Device in use"}) ==
          "Error starting stream: Unanticipated host error [PaErrorCode -9999]: 'Device in use' "
          "[Windows WASAPI error -2004287478]");
}
