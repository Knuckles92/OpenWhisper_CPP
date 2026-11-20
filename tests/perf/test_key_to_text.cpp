// Key to text (architecture 12.3: "Stop to paste ... p95 under 300 ms after post-roll for a 10 s
// dictation"). The controller's real feature graph offscreen (tests/ui_controller's
// ControllerRig): kp * through the fake keyboard hook starts the recorder on the fake
// microphone, 10 s of capture blocks arrive, kp * again stops it; the time is from that second
// press and release to the paste reaching the fake synthetic keys, minus the recorder's
// post-roll (50 ms in the rig), over 30 dictations; p50 and p95.
//
// The fake-engine case measures OpenWhisper_CPP's own path (journal, WAV, decode job, cleanup off,
// clipboard staging, paste). The [decode] case runs the same path with ParakeetBackend on the
// runtime and model of OPENWHISPER_CPP_TEST_RUNTIME_DIR and OPENWHISPER_CPP_TEST_MODEL, speaking
// OPENWHISPER_CPP_TEST_WAV; on a CPU runtime its number is the CPU's, not architecture 12.3's GPU
// figure, which stays the owner's check.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../ui_controller/controller_support.h"
#include "base/utf8.h"
#include "speech/parakeet_backend.h"
#include "support/perf_support.h"

namespace {

using namespace std::chrono_literals;
using fvtest::ControllerRig;
using fvtest::waitUntil;

constexpr double kPostRollMs = 50.0; // the rig's RecorderOptions::timings.post_roll
constexpr int kRecorderRate = 44100;

std::vector<std::int16_t> toneSeconds(double seconds) {
    const auto pcm =
        fv::audio::testing::read_pcm(ControllerRig::testkit_audio("tone-44k1-mono.wav"));
    const auto wanted = static_cast<std::size_t>(seconds * kRecorderRate);
    std::vector<std::int16_t> out;
    while (out.size() < wanted) {
        out.insert(out.end(), pcm.begin(), pcm.end());
    }
    out.resize(wanted);
    return out;
}

// A mono WAV of `rate` Hz (read_pcm keeps the file's rate) brought to the recorder's 44.1 kHz by
// linear interpolation: good enough speech for a recogniser, and no resampler to link.
std::vector<std::int16_t> to44k1(const std::vector<std::int16_t>& pcm, int rate) {
    if (rate == kRecorderRate || pcm.empty()) {
        return pcm;
    }
    const double step = static_cast<double>(rate) / kRecorderRate;
    const auto frames = static_cast<std::size_t>(static_cast<double>(pcm.size()) / step);
    std::vector<std::int16_t> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double at = static_cast<double>(i) * step;
        const auto low = static_cast<std::size_t>(at);
        const std::size_t high = std::min(low + 1, pcm.size() - 1);
        const double fraction = at - static_cast<double>(low);
        out[i] = static_cast<std::int16_t>(
            std::lround(pcm[low] * (1.0 - fraction) + pcm[high] * fraction));
    }
    return out;
}

int wavRate(const std::filesystem::path& file) {
    std::ifstream wav(file, std::ios::binary);
    char header[28] = {};
    wav.read(header, sizeof header);
    if (wav.gcount() < 28) {
        return 0;
    }
    return static_cast<unsigned char>(header[24]) | (static_cast<unsigned char>(header[25]) << 8) |
           (static_cast<unsigned char>(header[26]) << 16) |
           (static_cast<unsigned char>(header[27]) << 24);
}

// `runs` dictations of `pcm`; each sample is release-to-paste minus the post-roll, in ms.
std::vector<double> dictate(ControllerRig& rig, const std::vector<std::int16_t>& pcm, int runs,
                            std::chrono::seconds decodeTimeout) {
    std::atomic<int> pastes{0};
    std::atomic<std::int64_t> pastedAt{0};
    rig.keys().on_send = [&](const std::string& what) {
        if (what == "paste") {
            pastedAt = fvperf::Clock::now().time_since_epoch().count();
            ++pastes;
        }
    };
    std::vector<double> samples;
    for (int run = 0; run < runs; ++run) {
        const int before = pastes.load();
        rig.pressKeypad("*");
        REQUIRE(waitUntil([&rig] { return rig.runtime().is_recording(); }));
        REQUIRE(waitUntil([&rig] { return !rig.audio().streams().empty(); }));
        REQUIRE(rig.audio().streams().back()->deliver_blocks(pcm) > 0);
        // The record shortcut ignores a press within 300 ms of the last (HOTKEY_DEBOUNCE_MS).
        waitUntil([] { return false; }, 350ms);

        const auto released = fvperf::Clock::now();
        rig.pressKeypad("*");
        REQUIRE(waitUntil([&] { return pastes.load() > before; }, decodeTimeout));
        const auto pasted = fvperf::Clock::time_point(fvperf::Clock::duration(pastedAt.load()));
        samples.push_back(std::chrono::duration<double, std::milli>(pasted - released).count() -
                          kPostRollMs);
        REQUIRE(waitUntil([&rig] { return !rig.runtime().has_active_job(); }, decodeTimeout));
        waitUntil([] { return false; }, 350ms);
    }
    rig.keys().on_send = {};
    return samples;
}

} // namespace

TEST_CASE("key to text with the fake engine", "[perf]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    const auto samples = dictate(rig, toneSeconds(10.0), 30, 30s);
    const std::string test = "perf.key to text with the fake engine";
    const fv::Json notes{{"dictation_s", 10}, {"post_roll_ms", kPostRollMs}, {"engine", "fake"}};
    const auto p95 = fvperf::record("key_to_text_p95_ms", "ms", samples, test, notes);
    CHECK(fvperf::underCeiling(p95, p95.p95));
    fvperf::record("key_to_text_p50_ms", "ms", samples, test, notes);
}

TEST_CASE("key to text with Parakeet", "[perf][decode]") {
    const auto runtime = fv::testkit::get_env("OPENWHISPER_CPP_TEST_RUNTIME_DIR");
    const auto model = fv::testkit::get_env("OPENWHISPER_CPP_TEST_MODEL");
    const auto wav = fv::testkit::get_env("OPENWHISPER_CPP_TEST_WAV");
    if (!runtime || !model || !wav) {
        SKIP("Set OPENWHISPER_CPP_TEST_RUNTIME_DIR, OPENWHISPER_CPP_TEST_MODEL and "
             "OPENWHISPER_CPP_TEST_WAV.");
    }
    const std::filesystem::path runtimeDir = fv::path_from_utf8(*runtime);
    const std::filesystem::path modelPath = fv::path_from_utf8(*model);
    const std::filesystem::path speech = fv::path_from_utf8(*wav);

    fvtest::RigOptions options;
    options.backends = [runtimeDir, modelPath](fv::speech::BackendFactory fallback) {
        return fv::speech::BackendFactory(
            [runtimeDir, modelPath, fallback](
                const fv::speech::BackendConfig& given) -> std::unique_ptr<fv::speech::Backend> {
                if (given.backend != "parakeet") {
                    return fallback(given);
                }
                fv::speech::BackendConfig config = given;
                config.runtime_dir = runtimeDir;
                config.model_path = modelPath;
                config.device = "cpu";
                config.requested_device = "cpu";
                return std::make_unique<fv::speech::ParakeetBackend>(config);
            });
    };
    ControllerRig rig(std::move(options));
    // The real model loads in seconds, not the fake engine's instant: startEngine's own wait is
    // too short, so the same conditions are awaited here.
    rig.mainWindowShown();
    REQUIRE(waitUntil(
        [&rig] {
            return rig.speech().readiness(fv::speech::Slot::Dictation).state ==
                       fv::speech::ReadyState::Ready &&
                   rig.app().reload_machine().settled() && rig.app().startup_database_ready();
        },
        120s));

    const auto pcm = to44k1(fv::audio::testing::read_pcm(speech), wavRate(speech));
    REQUIRE_FALSE(pcm.empty());
    const auto samples = dictate(rig, pcm, 5, 120s);
    const std::string test = "perf.key to text with Parakeet";
    const fv::Json notes{{"dictation_s", static_cast<double>(pcm.size()) / kRecorderRate},
                         {"post_roll_ms", kPostRollMs},
                         {"engine", "ParakeetBackend"},
                         {"device", "cpu"},
                         {"model", modelPath.filename().string()}};
    const auto recorded = fvperf::record("key_to_text_decode_p95_ms", "ms", samples, test, notes);
    CHECK(fvperf::underCeiling(recorded, recorded.p95));
}
