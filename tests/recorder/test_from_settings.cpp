// The old microphone index migrates once it names a microphone, never as "none": a Bluetooth or
// USB microphone may not be connected yet when the first upgraded launch runs
// (tests/test_personalize_fix_f3_legacy_mic.py).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <thread>
#include <vector>

#include "audio/devices.h"
#include "base/executor.h"
#include "recorder_test_support.h"
#include "settings/testing/memory_store.h"

namespace {

using namespace fv::recorder_test;
using fv::Json;
using fv::Recorder;

const Json kLegacy = Json{{"audio_input_device", 2}};
const Json kUsbEntry = Json::array({{{"name", kUsb.name}, {"hostapi", kUsb.hostapi}}});

// windows_sd() without the USB microphone at index 2.
void unplug(fv::audio::testing::FakeBackend& fake) {
    fake.keep_devices(2);
}

} // namespace

// from
// tests/test_personalize_fix_f3_legacy_mic.py::test_an_unplugged_legacy_microphone_is_migrated_once_it_is_back
TEST_CASE("an unplugged legacy microphone is migrated once it is back",
          "[test_personalize_fix_f3_legacy_mic]") {
    fv::MemorySettingsStore store(kLegacy);
    auto unplugged = windows_fake();
    unplug(*unplugged);

    CHECK(fv::audio::load_priority(store, *unplugged).empty());
    CHECK_FALSE(store.contents().contains("audio_input_priority"));

    CHECK(fv::audio::load_priority(store, *windows_fake()) ==
          std::vector<fv::audio::DeviceKey>{kUsb});
    CHECK(store.contents()["audio_input_priority"] == kUsbEntry);
    CHECK(store.contents()["audio_input_device"] == 2);
}

// from
// tests/test_personalize_fix_f3_legacy_mic.py::test_from_settings_never_lists_devices_on_the_calling_thread
TEST_CASE("from settings never lists devices on the calling thread",
          "[test_personalize_fix_f3_legacy_mic]") {
    Rig rig;
    fv::MemorySettingsStore store(kLegacy);
    fv::ThreadPool pool("fv-test-migration", 1);

    auto recorder = Recorder::from_settings(store, rig.backend, rig.paths, pool);

    CHECK(wait_until(
        [&] { return recorder->device_priority() == std::vector<fv::audio::DeviceKey>{kUsb}; }));
    CHECK(store.contents()["audio_input_priority"] == kUsbEntry);
    const auto callers = rig.backend.listing_threads();
    CHECK_FALSE(callers.empty());
    CHECK(std::ranges::find(callers, std::this_thread::get_id()) == callers.end());
}

// from
// tests/test_personalize_fix_f3_legacy_mic.py::test_a_recorder_built_while_the_microphone_is_unplugged_keeps_trying
TEST_CASE("a recorder built while the microphone is unplugged keeps trying",
          "[test_personalize_fix_f3_legacy_mic]") {
    Rig rig;
    fv::MemorySettingsStore store(kLegacy);
    unplug(rig.backend.fake());
    {
        fv::ThreadPool pool("fv-test-migration", 1);
        auto first = Recorder::from_settings(store, rig.backend, rig.paths, pool);
        // The migration ran (it listed the devices) and found nothing at the old index.
        REQUIRE(wait_until([&] { return !rig.backend.listing_threads().empty(); }));
        pool.shutdown(std::chrono::milliseconds(3000));
        CHECK(first->device_priority().empty());
        CHECK_FALSE(store.contents().contains("audio_input_priority"));
    }

    rig.backend.fake().load(Json::parse(std::ifstream(fixture("devices-windows.json"))));
    fv::ThreadPool pool("fv-test-migration", 1);
    auto second = Recorder::from_settings(store, rig.backend, rig.paths, pool);
    CHECK(wait_until(
        [&] { return second->device_priority() == std::vector<fv::audio::DeviceKey>{kUsb}; }));
    CHECK(store.contents()["audio_input_priority"] == kUsbEntry);
}
