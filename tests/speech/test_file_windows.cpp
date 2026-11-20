// services/local_asr/audio.py windows(): speech::FileWindows against the golden vectors of
// services.local_asr.audio and the windows tests of tests/test_local_asr.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include "audio/file_decoder.h"
#include "audio/resampler.h"
#include "audio/wav_metadata.h"
#include "dsp/window_splitter.h"
#include "engine/error.h"
#include "golden/golden.h"
#include "speech/file_windows.h"
#include "speech_test_support.h"
#include "testkit/testkit.h"

using fv::speech::AudioClip;
using fv::speech::test::fixture;

namespace {

std::span<const float> samples_of(const AudioClip& clip) {
    return {clip.samples->data() + clip.begin, clip.end - clip.begin};
}

std::vector<float> joined(const std::vector<AudioClip>& clips) {
    std::vector<float> all;
    for (const AudioClip& clip : clips) {
        const auto samples = samples_of(clip);
        all.insert(all.end(), samples.begin(), samples.end());
    }
    return all;
}

// The recorder's path to the same windows: interleaved PCM through Resampler16k and a
// WindowSplitter, without the container.
std::vector<fv::dsp::Window> pcm_windows(std::span<const std::int16_t> pcm, int rate,
                                         int channels) {
    fv::audio::Resampler16k resampler(rate, channels);
    std::vector<std::int16_t> out;
    resampler.push(pcm, out);
    resampler.finish(out);
    fv::dsp::WindowSplitter splitter;
    auto windows = splitter.push(out);
    for (auto& window : splitter.finish()) {
        windows.push_back(std::move(window));
    }
    return windows;
}

// _write_recording: 16-bit PCM at 44.1 kHz with Quick Record's origination tags.
std::vector<std::int16_t> write_recording(const std::filesystem::path& path, double seconds,
                                          int channels) {
    const auto frames = static_cast<std::size_t>(seconds * 44100);
    std::vector<std::int16_t> samples = fv::speech::test::lcg_int16(frames * channels, 5);
    for (auto& sample : samples) {
        sample = static_cast<std::int16_t>(sample % 9000);
    }
    fv::speech::test::write_wav(path, 44100, channels, samples);
    const auto file = fv::audio::open_riff_file(path);
    fv::audio::append_origination(*file,
                                  std::chrono::local_days{std::chrono::year{2026} / 11 / 12} +
                                      std::chrono::hours{9} + std::chrono::minutes{30});
    return samples;
}

} // namespace

TEST_CASE("golden windows", "[golden]") {
    // Files OpenWhisper_CPP's FFmpeg decodes slightly differently from PyAV's (av.resample16k's
    // tolerance pins): their windows are compared by count and offset only.
    const fv::Json& pins = fv::golden::load("av.resample16k").at("pins");
    const fv::Json& tolerance = pins.at("tolerance");
    const auto inexact = [&](const std::string& source) {
        if (tolerance.contains(source)) {
            return true;
        }
#ifndef _WIN32
        if (pins.contains("tolerance_non_windows") &&
            pins.at("tolerance_non_windows").contains(source)) {
            return true;
        }
#endif
        return false;
    };
    fv::testkit::TempDataRoot root;
    fv::golden::for_each("services.local_asr.audio", "windows", [&](const fv::golden::Case& c) {
        const std::string source = c.arg<std::string>(0);
        std::filesystem::path path;
        if (source == "wav") {
            const std::string kind = c.arg<std::string>(1);
            const int rate = c.arg<int>(2);
            const int channels = c.arg<int>(3);
            const auto frames = c.arg<std::size_t>(4);
            const auto seed = c.arg<std::int64_t>(5);
            path = root.path() / (kind + "-" + std::to_string(rate) + ".wav");
            const auto samples = fv::speech::test::stream_signal(kind, frames * channels, seed);
            fv::speech::test::write_wav(path, rate, channels, samples);
        } else {
            path = fixture(source);
        }
        const fv::Json expected = c.result<fv::Json>();
        const std::vector<AudioClip> windows = fv::speech::file_windows(path);
        REQUIRE(windows.size() == expected.size());
        const bool exact = !inexact(source);
        for (std::size_t i = 0; i < windows.size(); ++i) {
            INFO(source << " window " << i);
            CHECK(windows[i].offset_s == expected[i].at(0).get<double>());
            CHECK(windows[i].rate == 16000);
            if (exact) {
                CHECK(windows[i].end - windows[i].begin == expected[i].at(1).get<std::size_t>());
                CHECK(fv::speech::test::digest(samples_of(windows[i])) ==
                      expected[i].at(2).get<std::string>());
            }
        }
    });
}

// from tests/test_local_asr.py::test_audio_windows_preserve_resampled_stereo_samples
TEST_CASE("audio windows preserve resampled stereo samples", "[test_local_asr]") {
    fv::testkit::TempDataRoot root;
    const auto path = root.path() / "audio.wav";
    constexpr int rate = 44100;
    std::vector<std::int16_t> stereo;
    stereo.reserve(static_cast<std::size_t>(rate) * 65 * 2);
    for (std::size_t i = 0; i < static_cast<std::size_t>(rate) * 65; ++i) {
        const double t = static_cast<double>(i) / rate;
        const auto value =
            static_cast<std::int16_t>(std::sin(2 * std::numbers::pi * 321 * t) * 16000);
        stereo.push_back(value);
        stereo.push_back(value);
    }
    fv::speech::test::write_wav(path, rate, 2, stereo);

    const std::vector<AudioClip> chunks = fv::speech::file_windows(path);
    const std::vector<float> all = joined(chunks);
    // faster_whisper's decode_audio(path, sampling_rate=16000): the same swresample conversion
    // of the interleaved samples, read without the splitter.
    fv::audio::AudioFileDecoder decoder(path);
    std::vector<std::int16_t> pcm;
    while (decoder.read_s16(pcm)) {
    }
    std::vector<float> expected;
    for (const auto& window : pcm_windows(pcm, rate, 2)) {
        expected.insert(expected.end(), window.samples.begin(), window.samples.end());
    }
    REQUIRE(all.size() == expected.size());
    float largest = 0.0f;
    for (std::size_t i = 0; i < all.size(); ++i) {
        largest = std::max(largest, std::fabs(all[i] - expected[i]));
    }
    CHECK(largest < 1e-4f);
    REQUIRE(chunks.size() >= 2);
    for (const AudioClip& chunk : chunks) {
        CHECK(chunk.end - chunk.begin <= 30u * 16000u);
    }
    CHECK(chunks[1].offset_s == static_cast<double>(chunks[0].end - chunks[0].begin) / 16000.0);
}

// from tests/test_local_asr.py::test_a_mono_recording_decodes_the_same_without_the_codec_search
// FFmpeg's options are fv_audio's (AudioFileDecoder opens a mono 16-bit WAV with probesize
// 4096; tests/audio pins is_mono_pcm16). Here: such a recording takes that path, and its
// windows are the ones its samples give without any container at all.
TEST_CASE("a mono recording decodes the same without the codec search", "[test_local_asr]") {
    const double seconds = GENERATE(0.4, 12.0, 61.0);
    fv::testkit::TempDataRoot root;
    const auto path = root.path() / "recorded.wav";
    const auto samples = write_recording(path, seconds, 1);
    REQUIRE(fv::audio::is_mono_pcm16(path));

    const std::vector<AudioClip> fast = fv::speech::file_windows(path);
    const auto direct = pcm_windows(samples, 44100, 1);
    REQUIRE(fast.size() == direct.size());
    std::size_t total = 0;
    for (std::size_t i = 0; i < fast.size(); ++i) {
        CHECK(fast[i].offset_s == direct[i].start_s);
        const auto window = samples_of(fast[i]);
        CHECK(std::ranges::equal(window, direct[i].samples));
        total += window.size();
    }
    CHECK(total == static_cast<std::size_t>(std::llround(seconds * 16000)));
}

// from tests/test_local_asr.py::test_a_stereo_file_keeps_ffmpegs_full_stream_analysis
TEST_CASE("a stereo file keeps ffmpegs full stream analysis", "[test_local_asr]") {
    fv::testkit::TempDataRoot root;
    const auto path = root.path() / "upload.wav";
    write_recording(path, 2.0, 2);
    CHECK_FALSE(fv::audio::is_mono_pcm16(path));
    CHECK_FALSE(fv::speech::file_windows(path).empty());
}

TEST_CASE("windows are read one at a time and end once") {
    fv::testkit::TempDataRoot root;
    const auto path = root.path() / "long.wav";
    const auto samples = fv::speech::test::stream_signal("gaps", 70 * 16000, 11);
    fv::speech::test::write_wav(path, 16000, 1, samples);
    fv::speech::FileWindows windows(path);
    std::size_t count = 0;
    double previous = -1.0;
    while (auto clip = windows.next()) {
        CHECK(clip->offset_s > previous);
        previous = clip->offset_s;
        ++count;
    }
    CHECK(count == 3);
    CHECK_FALSE(windows.next());
}

TEST_CASE("a file that cannot be decoded is an engine error") {
    fv::testkit::TempDataRoot root;
    CHECK_THROWS_AS(fv::speech::FileWindows(root.path() / "missing.wav"), fv::EngineError);
}
