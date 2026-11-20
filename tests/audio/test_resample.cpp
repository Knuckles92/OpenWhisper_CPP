// The 16 kHz decode path (services/local_asr/audio.py windows()): AudioFileDecoder and
// Resampler16k against PyAV's AudioResampler(format="s16", layout="mono", rate=16000).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/file_decoder.h"
#include "audio/resampler.h"
#include "audio/testing/fake_backend.h"
#include "audio_test_support.h"
#include "golden/golden.h"

namespace {

using fv::audio::test::fixture;

std::vector<std::int16_t> from_base64(const std::string& text) {
    static const std::string kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::uint8_t> bytes;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const auto value = kAlphabet.find(c);
        if (value == std::string::npos) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFFu));
        }
    }
    std::vector<std::int16_t> samples(bytes.size() / 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    }
    return samples;
}

struct Decoded {
    int rate = 0;
    int channels = 0;
    std::int64_t decoded_frames = 0;
    std::vector<std::int16_t> pcm16k;
};

// windows()'s loop without the splitter: every frame through one resampler, then the flush.
Decoded decode_16k(const std::string& name) {
    fv::audio::AudioFileDecoder decoder(fixture(name));
    fv::audio::Resampler16k resampler;
    fv::audio::DecodedFrame frame;
    Decoded result;
    result.rate = decoder.rate();
    result.channels = decoder.channels();
    while (decoder.next(frame)) {
        result.decoded_frames += static_cast<std::int64_t>(frame.frames());
        resampler.push(frame, result.pcm16k);
    }
    resampler.finish(result.pcm16k);
    return result;
}

std::vector<std::int16_t> resample_in_blocks(const std::vector<std::int16_t>& pcm, int rate,
                                             const std::vector<std::size_t>& blocks) {
    fv::audio::Resampler16k resampler(rate);
    std::vector<std::int16_t> out;
    std::size_t at = 0;
    std::size_t next = 0;
    while (at < pcm.size()) {
        const std::size_t take = std::min(blocks[next++ % blocks.size()], pcm.size() - at);
        resampler.push(std::span(pcm).subspan(at, take), out);
        at += take;
    }
    resampler.finish(out);
    return out;
}

} // namespace

TEST_CASE("golden resample16k", "[golden]") {
    // Exact unless the vector file states a tolerance for the file (pins.tolerance), where the
    // FFmpeg FioriVoce builds with decodes differently from the one PyAV bundles.
    const fv::Json& pins = fv::golden::load("av.resample16k").at("pins");
    fv::golden::for_each("av.resample16k", "resample16k", [&](const fv::golden::Case& c) {
        const std::string name = c.arg<std::string>(0);
        const fv::Json expected = c.result<fv::Json>();
        fv::Json tolerance = fv::Json::object();
        if (pins.contains("tolerance") && pins.at("tolerance").contains(name)) {
            tolerance = pins.at("tolerance").at(name);
        }
#ifndef _WIN32
        // The Linux build of the same FFmpeg rounds a few float samples differently.
        if (pins.contains("tolerance_non_windows") &&
            pins.at("tolerance_non_windows").contains(name)) {
            tolerance.update(pins.at("tolerance_non_windows").at(name));
        }
#endif
        const auto allowed = [&](const char* key) {
            return tolerance.value(key, std::int64_t{0});
        };

        const Decoded actual = decode_16k(name);
        CHECK(actual.rate == expected.at("rate").get<int>());
        CHECK(actual.channels == expected.at("channels").get<int>());
        const auto reference = from_base64(expected.at("pcm_s16le_base64").get<std::string>());
        REQUIRE(reference.size() == expected.at("samples").get<std::size_t>());
        const auto frames_apart =
            std::abs(actual.decoded_frames - expected.at("decoded_frames").get<std::int64_t>());
        const auto length_apart = std::abs(static_cast<std::int64_t>(actual.pcm16k.size()) -
                                           static_cast<std::int64_t>(reference.size()));
        std::int64_t differing = 0;
        std::int64_t largest = 0;
        std::size_t first_difference = reference.size();
        const std::size_t common = std::min(actual.pcm16k.size(), reference.size());
        for (std::size_t i = 0; i < common; ++i) {
            const int delta = std::abs(actual.pcm16k[i] - reference[i]);
            if (delta != 0) {
                ++differing;
                first_difference = std::min(first_difference, i);
            }
            largest = std::max<std::int64_t>(largest, delta);
        }
        INFO(name << ": decoded " << frames_apart << " frames apart, " << length_apart
                  << " samples apart in length; " << differing << " of " << common
                  << " samples differ, the first at " << first_difference << ", by at most "
                  << largest);
        CHECK(frames_apart <= allowed("decoded_frames"));
        CHECK(length_apart <= allowed("length"));
        CHECK(differing <= allowed("differing_samples"));
        CHECK(largest <= allowed("max_delta"));
        if (differing > 0) {
            CHECK(static_cast<std::int64_t>(first_difference) >=
                  static_cast<std::int64_t>(common) - allowed("differing_tail"));
        }
    });
}

TEST_CASE("recorder pcm through the resampler matches the decoded recording bit for bit") {
    // Incremental dictation feeds recorder PCM through its own resampler and relies on matching
    // windows() of the saved WAV exactly (local_asr/audio.py resampler()).
    const auto pcm = fv::audio::testing::read_pcm(fixture("tone-44k1-mono.wav"));
    const Decoded from_file = decode_16k("tone-44k1-mono.wav");
    CHECK(resample_in_blocks(pcm, 44100, {1024}) == from_file.pcm16k);
}

TEST_CASE("the resampled stream does not depend on how the input is cut") {
    const auto pcm = fv::audio::testing::read_pcm(fixture("tone-44k1-mono.wav"));
    const auto whole = resample_in_blocks(pcm, 44100, {pcm.size()});
    CHECK(resample_in_blocks(pcm, 44100, {1}) == whole);
    CHECK(resample_in_blocks(pcm, 44100, {1024}) == whole);
    CHECK(resample_in_blocks(pcm, 44100, {7, 4096, 333, 1, 1500}) == whole);
}

TEST_CASE("interleaved stereo pcm resamples as its decoded frames do") {
    const auto pcm = fv::audio::testing::read_pcm(fixture("tone-48k-stereo.wav"));
    fv::audio::Resampler16k resampler(48000, 2);
    std::vector<std::int16_t> out;
    resampler.push(pcm, out);
    resampler.finish(out);
    CHECK(out == decode_16k("tone-48k-stereo.wav").pcm16k);
}

TEST_CASE("16 kHz mono int16 passes through unchanged") {
    const auto pcm = fv::audio::testing::read_pcm(fixture("tone-16k-mono.wav"));
    CHECK(resample_in_blocks(pcm, 16000, {1000}) == pcm);
    CHECK(decode_16k("tone-16k-mono.wav").pcm16k == pcm);
}

TEST_CASE("a frame that does not match the first one is refused") {
    fv::audio::AudioFileDecoder mono(fixture("tone-44k1-mono.wav"));
    fv::audio::AudioFileDecoder stereo(fixture("tone-48k-stereo.wav"));
    fv::audio::Resampler16k resampler;
    fv::audio::DecodedFrame frame;
    std::vector<std::int16_t> out;
    REQUIRE(mono.next(frame));
    resampler.push(frame, out);
    REQUIRE(stereo.next(frame));
    CHECK_THROWS_WITH(resampler.push(frame, out), "Frame does not match AudioResampler setup.");
}

TEST_CASE("pushing after finish is refused") {
    fv::audio::Resampler16k resampler(44100);
    std::vector<std::int16_t> out;
    resampler.finish(out);
    const std::vector<std::int16_t> block(16, 0);
    CHECK_THROWS_AS(resampler.push(block, out), std::logic_error);
    CHECK_THROWS_AS(resampler.push(std::vector<std::int16_t>(3, 0), out), std::logic_error);
}
