// The real PortAudio backend. Listing devices and checking a format open no stream, so the
// [platform] test runs unattended; anything that captures from or plays to a real device is
// [interactive] (architecture amendment 14) and runs only in the windows-interactive preset.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "audio/devices.h"
#include "audio/player.h"
#include "audio_test_support.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "settings/store.h"

namespace {

using namespace std::chrono_literals;

void print_device(const fv::audio::InputDevice& device) {
    std::cout << "  [" << device.index << "] " << device.name << " | "
              << fv::audio::hostapi_label(device.hostapi) << " | " << device.channels << " ch "
              << device.samplerate << " Hz" << (device.is_default ? " | default" : "")
              << (device.role.empty() ? "" : " | role " + device.role) << '\n';
}

} // namespace

TEST_CASE("the portaudio input list and the ranked candidates for the saved priority",
          "[platform]") {
    const auto backend = fv::audio::make_portaudio_backend();
    std::vector<fv::audio::InputDevice> devices;
    try {
        devices = fv::audio::list_input_devices(*backend);
    } catch (const std::exception& error) {
        SKIP("PortAudio could not list devices: " << error.what());
    }
    std::cout << "PortAudio inputs:\n";
    for (const auto& device : devices) {
        print_device(device);
    }
    for (const auto& device : devices) {
        CHECK(device.name.find(fv::audio::kLoopbackMarker) == std::string::npos);
        CHECK_FALSE(device.hostapi.empty());
    }

    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    fv::SettingsStore settings(paths.user_file(fv::identity().settings_file));
    const auto priority = fv::audio::load_priority(settings, *backend);
    std::cout << "Saved priority: " << fv::audio::priority_json(priority).dump() << '\n';
    const auto ranked = fv::audio::ranked_candidates(*backend, priority, devices, {});
    std::cout << "Ranked candidates:\n";
    for (const auto& device : ranked) {
        print_device(device);
    }
    if (fv::audio::default_input(devices)) {
        // A default input that accepts 44.1 kHz mono int16 is always a candidate.
        CHECK_FALSE(ranked.empty());
    }
}

TEST_CASE("a short capture from the first ranked microphone", "[interactive]") {
    const auto backend = fv::audio::make_portaudio_backend();
    const auto ranked = fv::audio::ranked_candidates(*backend, {});
    REQUIRE_FALSE(ranked.empty());
    struct Counter {
        std::atomic<std::size_t> frames{0};
        std::atomic<int> overflows{0};
    } counter;
    const fv::audio::CaptureFn fn = [](void* user, const std::int16_t*, std::size_t count,
                                       fv::audio::BlockStatus status) {
        auto* c = static_cast<Counter*>(user);
        c->frames += count;
        c->overflows += status.overflow ? 1 : 0;
    };
    auto stream = backend->open_capture(fv::audio::input_config(&ranked.front()), fn, &counter, {});
    REQUIRE(stream);
    CHECK(fv::audio::open_stream_count() == 1);
    CHECK_FALSE(backend->refresh());
    std::this_thread::sleep_for(1s);
    CHECK((*stream)->active());
    (*stream)->abort();
    stream->reset();
    CHECK(counter.frames > 30000);
    CHECK(fv::audio::open_stream_count() == 0);
}

TEST_CASE("history playback through the default output", "[interactive]") {
    fv::audio::Player player(fv::audio::make_portaudio_backend());
    std::atomic<int> finished{0};
    REQUIRE(player.play(fv::audio::test::fixture("tone-44k1-mono.wav"), [&] { ++finished; }));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (finished == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(finished == 1);
    CHECK(player.last_error().empty());
    CHECK_FALSE(player.is_playing());
}
