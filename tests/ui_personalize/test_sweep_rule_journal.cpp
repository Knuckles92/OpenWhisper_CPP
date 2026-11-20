// A rule dictation releases its capture with the journal discarded: the words dictated for a
// cleanup rule are not a dictation to recover, so no recording journal folder stays behind
// (OpenWhisper's settings_cleanup_rules.py calls clear_recording_data() and leaves one per take).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "audio/testing/fake_backend.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "recorder/journal.h"
#include "recorder/recorder.h"
#include "testkit/testkit.h"
#include "ui_personalize/rule_dictation.h"

namespace {

std::size_t entries(const std::filesystem::path& folder) {
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
        return 0;
    }
    std::size_t count = 0;
    for (auto it = std::filesystem::directory_iterator(folder, error);
         !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("sweep: a rule dictation leaves no recording journal behind", "[widget]") {
    const fv::testkit::TempDataRoot root;
    const fv::MapEnvReader env{
        {std::string(fv::identity().env_prefix) + "DATA_DIR",
         fv::base_utf8::path_to_utf8(root.path())},
        {"LOCALAPPDATA", fv::base_utf8::path_to_utf8(root.path() / "local")},
        {"XDG_DATA_HOME", fv::base_utf8::path_to_utf8(root.path() / "local")},
        {"HOME", fv::base_utf8::path_to_utf8(root.path() / "home")},
        {"USERPROFILE", fv::base_utf8::path_to_utf8(root.path() / "home")}};
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), env);
    auto microphone = std::make_shared<fv::audio::testing::FakeBackend>();
    microphone->set_layout({fv::audio::testing::FakeHostApi{"MME", 0, -1}},
                           {fv::audio::testing::FakeDevice{"USB microphone", 0, 1, 0, 44100.0}}, 0);
    const std::filesystem::path audio = root.path() / "rule_dictation.wav";
    fv::RecorderOptions options;
    options.output_file = audio;
    options.timings.post_roll = std::chrono::milliseconds(50);
    fv::Recorder recorder(*microphone, paths, std::move(options));

    REQUIRE(recorder.start_recording());
    REQUIRE_FALSE(microphone->streams().empty());
    microphone->streams().back()->deliver(std::vector<std::int16_t>(44100, 1200));
    REQUIRE(recorder.sync_capture());
    REQUIRE(entries(fv::recovery_directory(audio)) > 0);

    const auto [text, error] = fv::transcribeRuleDictation(
        recorder, [](const std::filesystem::path&) { return std::string("never say literally"); },
        audio);

    CHECK(error.empty());
    CHECK(text == "never say literally");
    CHECK(entries(fv::recovery_directory(audio)) == 0);
    CHECK_FALSE(std::filesystem::exists(audio));
}
