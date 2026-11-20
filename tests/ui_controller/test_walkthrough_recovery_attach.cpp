// Startup recovery joins an interrupted walkthrough to its recovered audio in the composed app
// (transcription.py recover_recordings' walkthrough half, reached because the controller feature
// gives the runtime walkthrough::captures_root): the draft gets audio.wav from the WAV its
// journal was recovered into, and the status names walkthroughs first.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>

#include "controller_support.h"
#include "history/manager.h"
#include "walkthrough/session.h"
#include "walkthrough/storage.h"
#include "walkthrough/testing/fake_shot_image.h"

namespace {

void putLe(std::string& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<char>(value >> (8 * i) & 0xFF));
    }
}

// One second of a 220 Hz tone, mono 16-bit PCM at 16 kHz.
void writeTone(const std::filesystem::path& path) {
    constexpr int rate = 16'000;
    std::string data;
    for (int i = 0; i < rate; ++i) {
        const double t = static_cast<double>(i) / rate;
        const auto sample =
            static_cast<std::int16_t>(8000.0 * std::sin(2.0 * std::numbers::pi * 220.0 * t));
        putLe(data, static_cast<std::uint16_t>(sample), 2);
    }
    std::string wav = "RIFF";
    putLe(wav, static_cast<std::uint32_t>(36 + data.size()), 4);
    wav += "WAVEfmt ";
    putLe(wav, 16, 4);
    putLe(wav, 1, 2); // PCM
    putLe(wav, 1, 2); // mono
    putLe(wav, rate, 4);
    putLe(wav, rate * 2, 4);
    putLe(wav, 2, 2);
    putLe(wav, 16, 2);
    wav += "data";
    putLe(wav, static_cast<std::uint32_t>(data.size()), 4);
    wav += data;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(wav.data(), static_cast<std::streamsize>(wav.size()));
}

} // namespace

TEST_CASE("startup recovery joins an interrupted walkthrough to its recovered audio in the app",
          "[widget]") {
    std::filesystem::path draft;
    fvtest::RigOptions options;
    options.beforeFeatures = [&draft](fv::AppContext& context) {
        // A walkthrough interrupted while recording: its draft and screenshot are on disk, and
        // the recording's journal was recovered into a WAV in the recordings folder.
        const std::string journal = "0123456789abcdef0123456789abcdef";
        fv::walkthrough::WalkthroughSession::Options sessionOptions;
        sessionOptions.started_at =
            std::chrono::local_days{std::chrono::year{2026} / std::chrono::October / 21} +
            std::chrono::hours(14) + std::chrono::minutes(32);
        sessionOptions.root = fv::walkthrough::captures_root(context.paths());
        fv::walkthrough::WalkthroughSession session(std::move(sessionOptions));
        session.checkpoint_recording(journal);
        REQUIRE(session.add_shot(std::make_shared<fv::walkthrough::FakeShotImage>(32, 24, 0xFF4F3A),
                                 1.0));
        session.saved_shots();
        session.end();
        draft = session.folder();
        const auto recordings = fv::HistoryManager::default_recordings_folder(context.paths());
        std::filesystem::create_directories(recordings);
        writeTone(recordings / ("recording_2026-10-21_14-40-00-recovered-" + journal + ".wav"));
    };
    fvtest::ControllerRig rig(options);
    REQUIRE_FALSE(std::filesystem::exists(draft / "audio.wav"));

    REQUIRE(rig.startEngine());

    CHECK(fvtest::waitUntil(
        [&draft] { return std::filesystem::is_regular_file(draft / "audio.wav"); }));
    CHECK(fvtest::waitUntil([&rig] {
        for (const auto& status : rig.statuses()) {
            if (status == "Recovered 1 walkthrough(s) — open Saved Walkthroughs to finish them") {
                return true;
            }
        }
        return false;
    }));
}
