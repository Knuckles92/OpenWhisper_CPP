// AudioFileDecoder: av.open(path).decode(audio=0) over the six accepted file types.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "audio/file_decoder.h"
#include "audio_test_support.h"
#include "testkit/testkit.h"

namespace {

using fv::audio::AudioDecodeError;
using fv::audio::AudioFileDecoder;
using fv::audio::test::fixture;

// The data chunk of a plain 44-byte-header WAV, as int16.
std::vector<std::int16_t> wav_data(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::int16_t> samples((bytes.size() - 44) / 2);
    std::memcpy(samples.data(), bytes.data() + 44, samples.size() * 2);
    return samples;
}

} // namespace

TEST_CASE("every accepted file type decodes with its own rate and channels") {
    // Durations as PyAV reads them from each header: every fixture holds 0.8 s, but the AAC
    // header counts its priming, the ASF one its preroll, and the Ogg stream header from PyAV's
    // Vorbis encoder states 2752 samples.
    const auto [name, rate, channels, duration] = GENERATE(table<std::string, int, int, double>({
        {"tone-44k1-mono.wav", 44100, 1, 0.8},
        {"tone-48k-stereo.wav", 48000, 2, 0.8},
        {"tone-16k-mono.wav", 16000, 1, 0.8},
        {"tone-44k1-stereo.mp3", 44100, 2, 0.8},
        {"tone-44k1-stereo.m4a", 44100, 2, 36304.0 / 44100},
        {"tone-44k1-stereo.ogg", 44100, 2, 2752.0 / 44100},
        {"tone-48k-mono.flac", 48000, 1, 0.8},
        {"tone-44k1-stereo.wma", 44100, 2, 0.835},
    }));
    AudioFileDecoder decoder(fixture(name));
    CHECK(decoder.rate() == rate);
    CHECK(decoder.channels() == channels);
    CHECK(std::abs(decoder.duration_s() - duration) < 1e-6);
    fv::audio::DecodedFrame frame;
    std::size_t frames = 0;
    while (decoder.next(frame)) {
        CHECK(frame.rate() == rate);
        CHECK(frame.channels() == channels);
        frames += frame.frames();
    }
    CHECK(frames > static_cast<std::size_t>(rate * 0.75));
    CHECK_FALSE(decoder.next(frame));
}

TEST_CASE("a wav decodes to the samples it holds") {
    for (const char* name : {"tone-44k1-mono.wav", "tone-48k-stereo.wav"}) {
        INFO(name);
        AudioFileDecoder decoder(fixture(name));
        std::vector<std::int16_t> samples;
        while (decoder.read_s16(samples)) {
        }
        CHECK(samples == wav_data(fixture(name)));
    }
}

TEST_CASE("compressed audio converts to int16 at its own rate and layout") {
    AudioFileDecoder decoder(fixture("tone-44k1-stereo.mp3"));
    std::vector<std::int16_t> samples;
    while (decoder.read_s16(samples)) {
    }
    CHECK(samples.size() % 2 == 0);
    CHECK(samples.size() / 2 > 35000);
    const auto peak = *std::max_element(samples.begin(), samples.end());
    CHECK(peak > 10000);
}

TEST_CASE("a missing file and a file without audio are refused with their names") {
    const fv::testkit::TempDataRoot root;
    const auto missing = root.path() / "missing.wav";
    try {
        AudioFileDecoder decoder(missing);
        FAIL("a missing file opened");
    } catch (const AudioDecodeError& error) {
        CHECK(std::string(error.what()).find("missing.wav") != std::string::npos);
    }
    const auto text = root.path() / "notes.wav";
    std::ofstream(text) << "not audio at all, just some words in a file";
    CHECK_THROWS_AS(AudioFileDecoder(text), AudioDecodeError);
}
