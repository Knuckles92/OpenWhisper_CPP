// Microphone identity, ranking and migration against audio::testing::FakeBackend, the port of
// OpenWhisper's fake sounddevice (tests/test_personalize_s7_audio_devices.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "audio/devices.h"
#include "audio_test_support.h"
#include "base/testing/manual_executor.h"
#include "golden/golden.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"

namespace {

using fv::Json;
using fv::audio::DeviceKey;
using fv::audio::InputDevice;
using fv::audio::RankOptions;
using fv::audio::test::fixture;
using fv::audio::test::windows_backend;
using fv::audio::testing::FakeBackend;

const std::string kMme = "MME";
const std::string kDsound = "Windows DirectSound";
const std::string kWasapi = "Windows WASAPI";

std::vector<std::pair<int, std::string>> indexes_and_roles(const std::vector<InputDevice>& ranked) {
    std::vector<std::pair<int, std::string>> result;
    for (const InputDevice& device : ranked) {
        result.emplace_back(device.index, device.role);
    }
    return result;
}

RankOptions on_windows() {
    RankOptions options;
    options.sound_mapper = true;
    return options;
}

const fv::audio::StreamConfig* check_of(const std::vector<fv::audio::StreamConfig>& checks,
                                        int index) {
    // dict(sd.checks)[index]: the last check of that device.
    const fv::audio::StreamConfig* found = nullptr;
    for (const auto& check : checks) {
        if (check.device_index == index) {
            found = &check;
        }
    }
    return found;
}

class RecordingLifecycle final : public fv::audio::PortAudioLifecycle {
public:
    void terminate() override { events.emplace_back("terminate"); }
    void initialize() override {
        events.emplace_back("initialize");
        if (failures > 0) {
            --failures;
            throw std::runtime_error("Error initializing PortAudio");
        }
    }
    std::vector<std::string> events;
    int failures = 0; // initialize() calls that throw
};

} // namespace

// from
// tests/test_personalize_s7_audio_devices.py::test_listing_skips_loopback_and_restores_names_mme_cut
TEST_CASE("listing skips loopback and restores names mme cut",
          "[test_personalize_s7_audio_devices]") {
    const auto devices = fv::audio::list_input_devices(*windows_backend());

    std::vector<int> indexes;
    for (const auto& device : devices) {
        indexes.push_back(device.index);
    }
    CHECK(indexes == std::vector<int>{0, 1, 2, 3, 4, 5, 7});
    const InputDevice& usb = devices[2];
    CHECK(usb.key() == DeviceKey{"Microphone (2- USB Audio Device", kMme});
    CHECK(usb.label == "Microphone (2- USB Audio Device)");
    CHECK(usb.display() == "Microphone (USB Audio Device)");
    CHECK(usb.default_api);
    CHECK_FALSE(devices[4].default_api);
}

// from tests/test_personalize_s7_audio_devices.py::test_matching_ladder
TEST_CASE("matching ladder", "[test_personalize_s7_audio_devices]") {
    const auto [saved, expected] = GENERATE(table<DeviceKey, int>({
        {{"Microphone (Blue Snowball )", "MME"}, 1},
        // Replugged into another port: Windows renumbered the duplicate name.
        {{"Microphone (3- USB Audio Device)", "Windows WASAPI"}, 5},
        // MME's 31-character cut of a name that has since lost its number.
        {{"Microphone (4- USB Audio Device", "MME"}, 2},
        // Saved on a host API this machine lacks: the default API's cut name wins over the
        // exact name on DirectSound and WASAPI.
        {{"Microphone (2- USB Audio Device)", "Windows WDM-KS"}, 2},
    }));
    const auto devices = fv::audio::list_input_devices(*windows_backend());
    const auto match = fv::audio::match_entry(saved, devices);
    REQUIRE(match);
    CHECK(match->index == expected);
}

// from tests/test_personalize_s7_audio_devices.py::test_matching_strips_alsa_hardware_numbers
TEST_CASE("matching strips alsa hardware numbers", "[test_personalize_s7_audio_devices]") {
    FakeBackend backend;
    backend.set_layout(
        {{"ALSA", 0, -1}},
        {{"default", 0, 2, 0, 44100}, {"USB PnP Sound Device: Audio (hw:3,0)", 0, 1, 0, 44100}}, 0);
    const auto devices = fv::audio::list_input_devices(backend);
    const auto match =
        fv::audio::match_entry({"USB PnP Sound Device: Audio (hw:2,0)", "ALSA"}, devices);
    REQUIRE(match);
    CHECK(match->index == 1);
}

// from tests/test_personalize_s7_audio_devices.py::test_matching_is_conservative
TEST_CASE("matching is conservative", "[test_personalize_s7_audio_devices]") {
    const std::string name = GENERATE(std::string("Microphone (Webcam)"),
                                      // a prefix, but not MME's 31-character cut
                                      std::string("Microphone (USB"), std::string());
    const auto devices = fv::audio::list_input_devices(*windows_backend());
    CHECK_FALSE(fv::audio::match_entry({name, kMme}, devices).has_value());
}

// from
// tests/test_personalize_s7_audio_devices.py::test_candidates_follow_the_order_then_default_then_sound_mapper
TEST_CASE("candidates follow the order then default then sound mapper",
          "[test_personalize_s7_audio_devices]") {
    const auto backend = windows_backend();
    const std::vector<DeviceKey> priority{
        {"Microphone (2- USB Audio Device)", kWasapi},
        {"Microphone (Gone)", kMme},
        {"Microphone (Blue Snowball )", kMme},
    };

    const auto ranked = fv::audio::ranked_candidates(*backend, priority, on_windows());

    CHECK(indexes_and_roles(ranked) ==
          std::vector<std::pair<int, std::string>>{{5, ""}, {1, ""}, {0, "mapper"}});
    const auto checks = backend->checks();
    const auto* wasapi_check = check_of(checks, 5);
    REQUIRE(wasapi_check != nullptr);
    CHECK(wasapi_check->wasapi_auto_convert);
    const auto* mme_check = check_of(checks, 1);
    REQUIRE(mme_check != nullptr);
    CHECK_FALSE(mme_check->wasapi_auto_convert);
}

// from
// tests/test_personalize_s7_audio_devices.py::test_candidates_skip_devices_that_refuse_the_format_and_excluded_keys
TEST_CASE("candidates skip devices that refuse the format and excluded keys",
          "[test_personalize_s7_audio_devices]") {
    const auto backend = windows_backend();
    backend->refuse_format(7);
    const std::vector<DeviceKey> priority{
        {"Headset (soundcore V20i)", kDsound},
        {"Microphone (2- USB Audio Device", kMme},
    };
    RankOptions options = on_windows();
    options.exclude = {{"Microphone (2- USB Audio Device", kMme}};

    const auto ranked = fv::audio::ranked_candidates(*backend, priority, options);

    CHECK(indexes_and_roles(ranked) ==
          std::vector<std::pair<int, std::string>>{{1, "default"}, {0, "mapper"}});
}

// from tests/test_personalize_s7_audio_devices.py::test_sound_mapper_is_windows_only
TEST_CASE("sound mapper is windows only", "[test_personalize_s7_audio_devices]") {
    RankOptions options;
    options.sound_mapper = false;
    const auto ranked = fv::audio::ranked_candidates(*windows_backend(), {}, options);
    CHECK(indexes_and_roles(ranked) == std::vector<std::pair<int, std::string>>{{1, "default"}});
    // The platform default is the same rule as sys.platform == "win32".
    CHECK(RankOptions{}.sound_mapper == fv::audio::kSoundMapperPlatform);
}

// from
// tests/test_personalize_s7_audio_devices.py::test_meeting_lookup_skips_the_format_check_and_excluded_devices
// Meeting Mode's find_mic_device is the first of ranked_candidates(check=False); the meeting
// module itself is M7's.
TEST_CASE("meeting lookup skips the format check and excluded devices",
          "[test_personalize_s7_audio_devices]") {
    const auto backend = windows_backend();
    const std::vector<DeviceKey> priority{{"Microphone (2- USB Audio Device)", kWasapi}};
    RankOptions options = on_windows();
    options.check = false;

    const auto chosen = fv::audio::ranked_candidates(*backend, priority, options);
    REQUIRE_FALSE(chosen.empty());
    CHECK(chosen.front().index == 5);
    CHECK(chosen.front().samplerate == 48000);
    options.exclude = {chosen.front().key()};
    const auto next = fv::audio::ranked_candidates(*backend, priority, options);
    REQUIRE_FALSE(next.empty());
    CHECK(next.front().index == 1);
    CHECK(backend->checks().empty());
}

// from
// tests/test_personalize_s7_audio_devices.py::test_legacy_index_migrates_once_and_keeps_the_old_key
TEST_CASE("legacy index migrates once and keeps the old key",
          "[test_personalize_s7_audio_devices]") {
    fv::MemorySettingsStore store(Json{{"audio_input_device", 2}});

    const auto first = fv::audio::load_priority(store, *windows_backend());
    FakeBackend empty;
    const auto again = fv::audio::load_priority(store, empty);

    const std::vector<DeviceKey> expected{{"Microphone (2- USB Audio Device", kMme}};
    CHECK(first == expected);
    CHECK(again == expected);
    const Json saved = store.contents();
    CHECK(saved["audio_input_priority"] == fv::audio::priority_json(expected));
    CHECK(saved["audio_input_device"] == 2);
}

// from
// tests/test_personalize_s7_audio_devices.py::test_a_stale_legacy_index_uses_the_system_default_without_saving_it
TEST_CASE("a stale legacy index uses the system default without saving it",
          "[test_personalize_s7_audio_devices]") {
    fv::MemorySettingsStore store(Json{{"audio_input_device", 6}}); // now a loopback device

    CHECK(fv::audio::load_priority(store, *windows_backend()).empty());
    CHECK_FALSE(store.contents().contains("audio_input_priority"));
}

// from
// tests/test_personalize_s7_audio_devices.py::test_migration_waits_when_portaudio_is_unavailable
TEST_CASE("migration waits when portaudio is unavailable", "[test_personalize_s7_audio_devices]") {
    fv::MemorySettingsStore store(Json{{"audio_input_device", 2}});
    FakeBackend broken;
    broken.fail_listing("no PortAudio");

    CHECK(fv::audio::load_priority(store, broken).empty());
    CHECK_FALSE(store.contents().contains("audio_input_priority"));
}

// from tests/test_personalize_s7_audio_devices.py::test_saved_order_is_cleaned_without_writing
TEST_CASE("saved order is cleaned without writing", "[test_personalize_s7_audio_devices]") {
    const Json good = {{"name", "USB mic"}, {"hostapi", kMme}};
    fv::MemorySettingsStore store(
        Json{{"audio_input_priority", Json::array({good, good, Json{{"name", ""}}, "junk",
                                                   Json{{"name", "Desk"}, {"hostapi", 3}}})},
             {"audio_input_device", 2}});
    const auto writes = store.file().writes();

    CHECK(fv::audio::load_priority(store, *windows_backend()) ==
          std::vector<DeviceKey>{{"USB mic", kMme}});
    CHECK(store.file().writes() == writes);
    CHECK(fv::audio::normalize_priority(Json()).empty());
}

// from tests/test_personalize_s7_audio_devices.py::test_refresh_waits_for_every_stream_to_close
TEST_CASE("refresh waits for every stream to close", "[test_personalize_s7_audio_devices]") {
    RecordingLifecycle lifecycle;
    const int stream = 0;
    fv::audio::register_stream(&stream);
    CHECK_FALSE(fv::audio::refresh_portaudio(lifecycle));
    CHECK(lifecycle.events.empty());
    fv::audio::unregister_stream(&stream);
    CHECK(fv::audio::open_stream_count() == 0);
    CHECK(fv::audio::refresh_portaudio(lifecycle));
    CHECK(lifecycle.events == std::vector<std::string>{"terminate", "initialize"});
}

TEST_CASE("a refresh retries a failed restart once, then raises") {
    RecordingLifecycle once;
    once.failures = 1;
    CHECK(fv::audio::refresh_portaudio(once));
    CHECK(once.events == std::vector<std::string>{"terminate", "initialize", "initialize"});

    RecordingLifecycle twice;
    twice.failures = 2;
    CHECK_THROWS_AS(fv::audio::refresh_portaudio(twice), std::runtime_error);
    CHECK(twice.events == std::vector<std::string>{"terminate", "initialize", "initialize"});
}

// from tests/test_personalize_s7_audio_devices.py::test_names_read_well
TEST_CASE("names read well", "[test_personalize_s7_audio_devices]") {
    const auto [name, shown, shorter] = GENERATE(table<std::string, std::string, std::string>({
        {"Microphone (2- USB Audio Device", "Microphone (USB Audio Device)", "USB Audio Device"},
        {"Microphone (Blue Snowball )", "Microphone (Blue Snowball)", "Blue Snowball"},
        {"Microphone Array (Realtek(R) Audio)", "Microphone Array (Realtek(R) Audio)",
         "Realtek(R) Audio"},
        {"MacBook Pro Microphone", "MacBook Pro Microphone", "MacBook Pro Microphone"},
        {"HDA Intel PCH: ALC892 Analog (hw:0,0)", "HDA Intel PCH: ALC892 Analog",
         "HDA Intel PCH: ALC892 Analog"},
    }));
    CHECK(fv::audio::display_name(name) == shown);
    CHECK(fv::audio::short_name(name) == shorter);
}

// from tests/test_personalize_s7_audio_devices.py::test_input_device_key_is_name_and_host_api_name
TEST_CASE("input device key is name and host api name", "[test_personalize_s7_audio_devices]") {
    InputDevice device;
    device.index = 4;
    device.name = "Desk";
    device.hostapi = kMme;
    CHECK(fv::audio::device_key(device) == DeviceKey{"Desk", kMme});
    CHECK(fv::audio::device_key(Json{{"name", "Desk"}, {"hostapi", kMme}, {"index", 4}}) ==
          device.key());
}

TEST_CASE("a migration pending on the gui thread runs on the migration executor") {
    fv::MemorySettingsStore store(Json{{"audio_input_device", 2}});
    const auto backend = windows_backend();
    fv::ManualExecutor migration;
    std::vector<std::vector<DeviceKey>> delivered;

    const auto now =
        fv::audio::load_priority(store, *backend, migration,
                                 [&](std::vector<DeviceKey> keys) { delivered.push_back(keys); });

    CHECK(now.empty());
    CHECK(backend->listings() == 0);
    migration.run_all();
    REQUIRE(delivered.size() == 1);
    CHECK(delivered[0] == std::vector<DeviceKey>{{"Microphone (2- USB Audio Device", kMme}});
    CHECK(backend->listings() == 1);

    // Nothing pending: the saved order at once, nothing posted, no device listed.
    const auto saved = fv::audio::load_priority(store, *backend, migration,
                                                [&](std::vector<DeviceKey>) { FAIL("posted"); });
    CHECK(saved == delivered[0]);
    CHECK(backend->listings() == 1);
}

TEST_CASE("the backend contract ladder ranks device infos without a format check") {
    const auto backend = windows_backend();
    const auto inputs = backend->inputs();
    const std::vector<DeviceKey> priority{{"Microphone (3- USB Audio Device)", kWasapi}};
    const auto ranked = fv::audio::ranked_candidates(priority, inputs);
    std::vector<int> indexes;
    for (const auto& info : ranked) {
        indexes.push_back(info.index);
    }
    std::vector<int> expected{5, 1};
    if (fv::audio::kSoundMapperPlatform) {
        expected.push_back(0);
    }
    CHECK(indexes == expected);
    CHECK(backend->checks().empty());
}

TEST_CASE("the fake backend refuses a refresh while a stream is open") {
    const auto backend = windows_backend();
    auto stream = backend->open_capture({.device_index = 1}, nullptr, nullptr, {});
    REQUIRE(stream);
    CHECK_FALSE(backend->refresh());
    stream->reset();
    CHECK(backend->refresh());
    CHECK(backend->lifecycle() == std::vector<std::string>{"terminate", "initialize"});
}

// ---- golden vectors: services.audio_devices ------------------------------------------------

namespace {

std::shared_ptr<FakeBackend> fixture_backend(const std::string& name) {
    return FakeBackend::from_fixture(fixture(name));
}

Json key_json(const DeviceKey& key) {
    return fv::audio::device_key_json(key);
}

} // namespace

TEST_CASE("golden display name, short name and host api label", "[golden]") {
    fv::golden::for_each("services.audio_devices", "display_name", [](const fv::golden::Case& c) {
        CHECK(fv::audio::display_name(c.arg<std::string>(0)) == c.result<std::string>());
    });
    fv::golden::for_each("services.audio_devices", "short_name", [](const fv::golden::Case& c) {
        CHECK(fv::audio::short_name(c.arg<std::string>(0)) == c.result<std::string>());
    });
    fv::golden::for_each("services.audio_devices", "hostapi_label", [](const fv::golden::Case& c) {
        CHECK(fv::audio::hostapi_label(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden device key and normalize priority", "[golden]") {
    fv::golden::for_each("services.audio_devices", "device_key", [](const fv::golden::Case& c) {
        CHECK(fv::golden::equal(c.result<Json>(), key_json(fv::audio::device_key(c.arg<Json>(0)))));
    });
    fv::golden::for_each(
        "services.audio_devices", "normalize_priority", [](const fv::golden::Case& c) {
            const Json actual =
                fv::audio::priority_json(fv::audio::normalize_priority(c.arg<Json>(0)));
            CHECK(fv::golden::equal(c.result<Json>(), actual));
        });
}

TEST_CASE("golden device listing and default input", "[golden]") {
    fv::golden::for_each(
        "services.audio_devices", "list_input_devices", [](const fv::golden::Case& c) {
            const auto devices =
                fv::audio::list_input_devices(*fixture_backend(c.arg<std::string>(0)));
            Json actual = Json::array();
            for (const auto& d : devices) {
                actual.push_back(Json{{"index", d.index},
                                      {"name", d.name},
                                      {"hostapi", d.hostapi},
                                      {"channels", d.channels},
                                      {"samplerate", d.samplerate},
                                      {"label", d.label},
                                      {"default_api", d.default_api},
                                      {"role", d.role},
                                      {"display", d.display()}});
            }
            CHECK(fv::golden::equal(c.result<Json>(), actual));
        });
    fv::golden::for_each("services.audio_devices", "default_input", [](const fv::golden::Case& c) {
        const auto devices = fv::audio::list_input_devices(*fixture_backend(c.arg<std::string>(0)));
        const auto found = fv::audio::default_input(devices);
        CHECK(c.result<std::optional<int>>() ==
              (found ? std::optional<int>(found->index) : std::nullopt));
    });
    fv::golden::for_each("services.audio_devices", "is_system_alias",
                         [](const fv::golden::Case& c) {
                             InputDevice device;
                             device.name = c.arg<std::string>(0);
                             device.hostapi = kMme;
                             CHECK(fv::audio::is_system_alias(device) == c.result<bool>());
                         });
}

TEST_CASE("golden match entry", "[golden]") {
    fv::golden::for_each("services.audio_devices", "match_entry", [](const fv::golden::Case& c) {
        const auto devices = fv::audio::list_input_devices(*fixture_backend(c.arg<std::string>(1)));
        const auto found = fv::audio::match_entry(fv::audio::device_key(c.arg<Json>(0)), devices);
        CHECK(c.result<std::optional<int>>() ==
              (found ? std::optional<int>(found->index) : std::nullopt));
    });
}

TEST_CASE("golden ranked candidates", "[golden]") {
    fv::golden::for_each(
        "services.audio_devices", "ranked_candidates", [](const fv::golden::Case& c) {
            const auto backend = fixture_backend(c.arg<std::string>(0));
            for (const int index : c.arg<std::vector<int>>(4)) {
                backend->refuse_format(index);
            }
            RankOptions options;
            options.sound_mapper = true; // generated on Windows
            options.exclude = fv::audio::normalize_priority(c.arg<Json>(2));
            options.check = c.arg<bool>(3);
            const auto priority = fv::audio::normalize_priority(c.arg<Json>(1));
            const auto ranked = fv::audio::ranked_candidates(*backend, priority, options);
            Json actual = Json::array();
            for (const auto& device : ranked) {
                actual.push_back(Json::array({device.index, device.role}));
            }
            CHECK(fv::golden::equal(c.result<Json>(), actual));
        });
}

TEST_CASE("golden legacy migration pending", "[golden]") {
    fv::golden::for_each(
        "services.audio_devices", "legacy_migration_pending", [](const fv::golden::Case& c) {
            CHECK(fv::audio::legacy_migration_pending(c.arg<Json>(0)) == c.result<bool>());
        });
}
