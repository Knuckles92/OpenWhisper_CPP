// Ported from OpenWhisper tests/test_meeting_capture_devices.py: Meeting Mode's device discovery
// over audio::testing::FakeBackend layouts in place of sounddevice stand-ins. sounddevice's
// default-device shapes (an _InputOutputPair, a list, a tuple, an unreadable object) are one
// thing in C++, PortAudio's default input index, so those cases run the same layout; the
// loopback cases use the backend's render endpoints (WASAPI loopback) instead of "[Loopback]"
// PortAudio inputs.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "audio/devices.h"
#include "audio/testing/fake_backend.h"
#include "capture_test_support.h"
#include "meeting_capture/devices.h"

using fv::audio::DeviceKey;
using fv::audio::LoopbackEndpoint;
using fv::audio::testing::FakeBackend;
using fv::audio::testing::FakeDevice;
using fv::audio::testing::FakeHostApi;
using fv::log::Level;
using fv::meeting::find_loopback_endpoint;
using fv::meeting::find_mic_device;
using fv::meeting::test::LogCapture;

namespace {

FakeDevice mic(std::string name, int channels = 1, int hostapi = 0, double rate = 48000) {
    return FakeDevice{.name = std::move(name),
                      .hostapi = hostapi,
                      .max_input_channels = channels,
                      .max_output_channels = 0,
                      .default_samplerate = rate};
}

// _fake_sd(devices, default=(input, output)): one "Core Audio" host API whose defaults are the
// pair; `portaudio_default` is the process-wide default input (-1: unreadable).
std::shared_ptr<FakeBackend> fake_backend(std::vector<FakeDevice> devices, int input = 1,
                                          int output = 2, int portaudio_default = -2) {
    auto backend = std::make_shared<FakeBackend>();
    backend->set_layout(
        {FakeHostApi{
            .name = "Core Audio", .default_input_device = input, .default_output_device = output}},
        std::move(devices), portaudio_default == -2 ? input : portaudio_default, 0, output);
    return backend;
}

struct FreshLogState {
    FreshLogState() { fv::meeting::forget_logged_probe_outcomes(); }
    ~FreshLogState() { fv::meeting::forget_logged_probe_outcomes(); }
};

std::shared_ptr<FakeBackend> wasapi_backend(std::vector<FakeDevice> devices) {
    auto backend = std::make_shared<FakeBackend>();
    backend->set_layout({FakeHostApi{.name = "Windows WASAPI",
                                     .default_input_device = 0,
                                     .default_output_device = 1}},
                        std::move(devices), 0, 0, 1);
    return backend;
}

LoopbackEndpoint endpoint(std::string id, std::string name, bool is_default) {
    return LoopbackEndpoint{.id = std::move(id),
                            .name = std::move(name),
                            .is_default = is_default,
                            .rate = 48000,
                            .channels = 2};
}

// The level the first "no render endpoint" outcome is logged at: a warning on Windows, where
// system audio is expected; debug elsewhere.
#ifdef _WIN32
constexpr Level kMissingLoopback = Level::Warning;
constexpr std::string_view kMissingLoopbackText = "render endpoints found";
#else
constexpr Level kMissingLoopback = Level::Debug;
constexpr std::string_view kMissingLoopbackText = "No WASAPI host API found";
#endif

} // namespace

// from
// tests/test_meeting_capture_devices.py::TestFindMicDevice::test_input_output_pair_uses_system_default_not_index_zero
TEST_CASE("TestFindMicDevice input output pair uses system default not index zero",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    auto backend = fake_backend({mic("Virus Probe Microphone"), mic("MacBook Air Microphone")});
    const auto chosen = find_mic_device(*backend, {});
    REQUIRE(chosen);
    CHECK(chosen->index == 1);
    CHECK(chosen->name == "MacBook Air Microphone");
}

// from
// tests/test_meeting_capture_devices.py::TestFindMicDevice::test_list_default_device_still_works
TEST_CASE("TestFindMicDevice list default device still works", "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    auto backend = fake_backend({mic("Probe"), mic("Built-in Microphone")});
    const auto chosen = find_mic_device(*backend, {});
    REQUIRE(chosen);
    CHECK(chosen->index == 1);
}

// from
// tests/test_meeting_capture_devices.py::TestFindMicDevice::test_tuple_default_device_still_works
TEST_CASE("TestFindMicDevice tuple default device still works", "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    // PortAudio's default given only through the host API, as a tuple would carry it.
    auto backend = fake_backend({mic("Probe"), mic("Built-in Microphone")}, 1, 2, 1);
    const auto chosen = find_mic_device(*backend, {});
    REQUIRE(chosen);
    CHECK(chosen->index == 1);
}

// from
// tests/test_meeting_capture_devices.py::TestFindMicDevice::test_preferred_index_wins_when_valid
TEST_CASE("TestFindMicDevice preferred index wins when valid", "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    auto backend = fake_backend({mic("USB Mic"), mic("Built-in Microphone")});
    const auto chosen = find_mic_device(*backend, {}, 0);
    REQUIRE(chosen);
    CHECK(chosen->index == 0);
    CHECK(chosen->name == "USB Mic");
}

// from
// tests/test_meeting_capture_devices.py::TestFindMicDevice::test_invalid_preferred_falls_back_to_default
TEST_CASE("TestFindMicDevice invalid preferred falls back to default",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    const LogCapture logs;
    auto backend = fake_backend({mic("Probe"), mic("Built-in Microphone")});
    const auto chosen = find_mic_device(*backend, {}, 99);
    REQUIRE(chosen);
    CHECK(chosen->index == 1);
    CHECK(logs.levels("Preferred mic device index 99 is not a valid input device") ==
          std::vector{Level::Warning});
}

// from
// tests/test_meeting_capture_devices.py::TestFindMicDevice::test_hostapi_default_used_when_pair_unreadable
TEST_CASE("TestFindMicDevice hostapi default used when pair unreadable",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    auto backend = fake_backend({mic("Probe"), mic("Built-in Microphone")}, 1, 2, -1);
    const auto chosen = find_mic_device(*backend, {});
    REQUIRE(chosen);
    CHECK(chosen->index == 1);
}

// from tests/test_meeting_capture_devices.py::TestFindMicDevice::test_skips_loopback_named_inputs
TEST_CASE("TestFindMicDevice skips loopback named inputs", "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    auto backend = fake_backend({mic("Speakers [Loopback]", 2), mic("Built-in Microphone")}, 0, 2);
    const auto chosen = find_mic_device(*backend, {});
    REQUIRE(chosen);
    CHECK(chosen->index == 1);
}

// from
// tests/test_meeting_capture_devices.py::TestDefaultDeviceHelpers::test_pair_indexes_are_read_without_type_check
TEST_CASE("TestDefaultDeviceHelpers pair indexes are read without type check",
          "[test_meeting_capture_devices]") {
    auto backend = fake_backend({mic("A"), mic("B"), mic("C"), mic("D"), mic("E")}, 3, 4);
    const auto inputs = backend->inputs();
    CHECK(fv::audio::default_input_index(inputs, 3, 4) == 3);
}

// from
// tests/test_meeting_capture_devices.py::TestDefaultDeviceHelpers::test_unreadable_pair_returns_nones
TEST_CASE("TestDefaultDeviceHelpers unreadable pair returns nones",
          "[test_meeting_capture_devices]") {
    auto backend = fake_backend({mic("A")}, -1, -1, -1);
    const auto inputs = backend->inputs();
    CHECK(fv::audio::default_input_index(inputs, -1, -1) == -1);
    CHECK_FALSE(find_loopback_endpoint(*backend));
}

// from
// tests/test_meeting_capture_devices.py::TestDefaultDeviceHelpers::test_default_output_name_accepts_pair
TEST_CASE("TestDefaultDeviceHelpers default output name accepts pair",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    auto backend = wasapi_backend({mic("Mic")});
    backend->set_loopbacks({endpoint("{mic}", "Mic", false), endpoint("{unused}", "Unused", false),
                            endpoint("{speakers}", "Speakers", true)});
    const auto chosen = find_loopback_endpoint(*backend);
    REQUIRE(chosen);
    CHECK(chosen->name == "Speakers");
}

// from
// tests/test_meeting_capture_devices.py::TestFindLoopbackDevice::test_missing_wasapi_is_not_an_error_off_windows
TEST_CASE("TestFindLoopbackDevice missing wasapi is not an error off windows",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    const LogCapture logs;
    auto backend = fake_backend({mic("Mic")});
    CHECK_FALSE(find_loopback_endpoint(*backend));
    CHECK(logs.of("meeting.capture.devices", Level::Error).empty());
}

// from
// tests/test_meeting_capture_devices.py::TestProbeLoggingIsRateLimited::test_missing_loopback_warns_once_per_outage
TEST_CASE("TestProbeLoggingIsRateLimited missing loopback warns once per outage",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    const LogCapture logs;
    auto backend = wasapi_backend({mic("Mic")});
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(find_loopback_endpoint(*backend));
    }
    CHECK(logs.levels(kMissingLoopbackText) ==
          std::vector{kMissingLoopback, Level::Debug, Level::Debug, Level::Debug, Level::Debug});
}

// from
// tests/test_meeting_capture_devices.py::TestProbeLoggingIsRateLimited::test_loopback_outage_is_logged_again_after_recovery
TEST_CASE("TestProbeLoggingIsRateLimited loopback outage is logged again after recovery",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    const LogCapture logs;
    auto backend = wasapi_backend({mic("Mic")});
    find_loopback_endpoint(*backend);
    backend->set_loopbacks({endpoint("{speakers}", "Speakers", true)});
    CHECK(find_loopback_endpoint(*backend));
    backend->set_loopbacks({});
    find_loopback_endpoint(*backend);
    CHECK(logs.levels(kMissingLoopbackText) == std::vector{kMissingLoopback, kMissingLoopback});
}

// from
// tests/test_meeting_capture_devices.py::TestProbeLoggingIsRateLimited::test_unchanged_mic_choice_logs_info_once
TEST_CASE("TestProbeLoggingIsRateLimited unchanged mic choice logs info once",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    const LogCapture logs;
    auto backend = fake_backend({mic("Probe"), mic("Built-in Microphone")});
    for (int i = 0; i < 4; ++i) {
        find_mic_device(*backend, {});
    }
    CHECK(logs.levels("Microphone device:") ==
          std::vector{Level::Info, Level::Debug, Level::Debug, Level::Debug});
}

// from
// tests/test_meeting_capture_devices.py::TestProbeLoggingIsRateLimited::test_changed_mic_choice_is_logged_again
TEST_CASE("TestProbeLoggingIsRateLimited changed mic choice is logged again",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    const LogCapture logs;
    auto backend = fake_backend({mic("USB Mic"), mic("Built-in Microphone")});
    find_mic_device(*backend, {});
    find_mic_device(*backend, {}, 0);
    CHECK(logs.levels("Microphone device:") == std::vector{Level::Info, Level::Info});
}

TEST_CASE("an excluded microphone is skipped and the ranked order comes first",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    auto backend = fake_backend({mic("USB Mic"), mic("Built-in Microphone"), mic("Headset")});
    const std::vector<DeviceKey> priority = {{"Headset", "Core Audio"}};
    const auto ranked = find_mic_device(*backend, priority);
    REQUIRE(ranked);
    CHECK(ranked->name == "Headset");
    const std::vector<DeviceKey> exclude = {{"Headset", "Core Audio"},
                                            {"Built-in Microphone", "Core Audio"}};
    const auto next = find_mic_device(*backend, priority, std::nullopt, exclude);
    REQUIRE(next);
    CHECK(next->name == "USB Mic");
    // Every input excluded: nothing is left.
    const std::vector<DeviceKey> all = {{"Headset", "Core Audio"},
                                        {"Built-in Microphone", "Core Audio"},
                                        {"USB Mic", "Core Audio"}};
    CHECK_FALSE(find_mic_device(*backend, priority, std::nullopt, all));
}

TEST_CASE("a device list that cannot be read is no device", "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    const LogCapture logs;
    auto backend = fake_backend({mic("Mic")});
    backend->fail_listing("PortAudio not initialized");
    backend->fail_loopback_listing("no endpoints");
    CHECK_FALSE(find_mic_device(*backend, {}));
    CHECK_FALSE(find_loopback_endpoint(*backend));
    CHECK(logs.levels("Microphone device discovery failed") == std::vector{Level::Error});
    CHECK(logs.levels("Loopback device discovery failed") == std::vector{Level::Error});
}

TEST_CASE("without a default render endpoint the first one is used",
          "[test_meeting_capture_devices]") {
    const FreshLogState fresh;
    auto backend = wasapi_backend({mic("Mic")});
    backend->set_loopbacks({endpoint("{a}", "Speakers", false), endpoint("{b}", "HDMI", false)});
    const auto chosen = find_loopback_endpoint(*backend);
    REQUIRE(chosen);
    CHECK(chosen->id == "{a}");
}
