#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "engine/error.h"
#include "engine/wav.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_tag(Bytes& out, std::string_view tag) {
    out.insert(out.end(), tag.begin(), tag.end());
}

void put_u16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_u32(Bytes& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

// A WAV file around `data`, with an odd-length chunk before the audio to
// exercise chunk skipping and padding.
Bytes make_wav(std::uint16_t format, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits,
               const Bytes& data) {
    Bytes out;
    put_tag(out, "RIFF");
    put_u32(out, 0); // readers must not depend on it
    put_tag(out, "WAVE");
    put_tag(out, "fmt ");
    put_u32(out, 16);
    put_u16(out, format);
    put_u16(out, channels);
    put_u32(out, rate);
    put_u32(out, rate * channels * bits / 8);
    put_u16(out, static_cast<std::uint16_t>(channels * bits / 8));
    put_u16(out, bits);
    put_tag(out, "LIST");
    put_u32(out, 3);
    out.insert(out.end(), {'a', 'b', 'c', 0});
    put_tag(out, "data");
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Bytes pcm16(std::initializer_list<std::int16_t> samples) {
    Bytes out;
    for (const std::int16_t sample : samples) {
        put_u16(out, static_cast<std::uint16_t>(sample));
    }
    return out;
}

} // namespace

TEST_CASE("16-bit mono PCM decodes to floats at its own rate") {
    const auto audio = fv::parse_wav(make_wav(1, 1, 44100, 16, pcm16({0, 16384, -32768, 32767})));
    CHECK(audio.sample_rate == 44100);
    REQUIRE(audio.samples.size() == 4);
    CHECK_THAT(audio.samples[0], WithinAbs(0.0, 1e-6));
    CHECK_THAT(audio.samples[1], WithinAbs(0.5, 1e-6));
    CHECK_THAT(audio.samples[2], WithinAbs(-1.0, 1e-6));
    CHECK_THAT(audio.samples[3], WithinAbs(32767.0 / 32768.0, 1e-6));
    CHECK_THAT(audio.duration(), WithinAbs(4.0 / 44100.0, 1e-9));
}

TEST_CASE("stereo is averaged to mono") {
    const auto audio = fv::parse_wav(make_wav(1, 2, 16000, 16, pcm16({16384, 0, -16384, -16384})));
    REQUIRE(audio.samples.size() == 2);
    CHECK_THAT(audio.samples[0], WithinAbs(0.25, 1e-6));
    CHECK_THAT(audio.samples[1], WithinAbs(-0.5, 1e-6));
}

TEST_CASE("8-bit, 24-bit and 32-bit PCM decode") {
    const auto eight = fv::parse_wav(make_wav(1, 1, 8000, 8, {128, 255, 0}));
    CHECK_THAT(eight.samples[0], WithinAbs(0.0, 1e-6));
    CHECK_THAT(eight.samples[1], WithinAbs(127.0 / 128.0, 1e-6));
    CHECK_THAT(eight.samples[2], WithinAbs(-1.0, 1e-6));

    const auto twenty_four =
        fv::parse_wav(make_wav(1, 1, 48000, 24, {0x00, 0x00, 0x40, 0x00, 0x00, 0x80}));
    CHECK_THAT(twenty_four.samples[0], WithinAbs(0.5, 1e-6));
    CHECK_THAT(twenty_four.samples[1], WithinAbs(-1.0, 1e-6));

    Bytes data;
    put_u32(data, 0x40000000u);
    put_u32(data, 0x80000000u);
    const auto thirty_two = fv::parse_wav(make_wav(1, 1, 48000, 32, data));
    CHECK_THAT(thirty_two.samples[0], WithinAbs(0.5, 1e-6));
    CHECK_THAT(thirty_two.samples[1], WithinAbs(-1.0, 1e-6));
}

TEST_CASE("32-bit float decodes unchanged") {
    const float values[] = {0.25f, -0.75f};
    Bytes data(sizeof(values));
    std::memcpy(data.data(), values, sizeof(values));
    const auto audio = fv::parse_wav(make_wav(3, 1, 16000, 32, data));
    REQUIRE(audio.samples.size() == 2);
    CHECK(audio.samples[0] == 0.25f);
    CHECK(audio.samples[1] == -0.75f);
}

TEST_CASE("a data chunk with no recorded size runs to the end of the file") {
    Bytes wav = make_wav(1, 1, 16000, 16, pcm16({1000, 2000, 3000}));
    // The size field is the four bytes before the six bytes of audio.
    std::memset(wav.data() + wav.size() - 6 - 4, 0xFF, 4);
    CHECK(fv::parse_wav(wav).samples.size() == 3);
}

TEST_CASE("a trailing partial frame is dropped") {
    Bytes data = pcm16({1000, 2000});
    data.push_back(0x7F);
    CHECK(fv::parse_wav(make_wav(1, 1, 16000, 16, data)).samples.size() == 2);
}

TEST_CASE("files that are not usable WAV audio are refused with a reason") {
    const Bytes not_wav = {'O', 'g', 'g', 'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_THROWS_WITH(fv::parse_wav(not_wav), ContainsSubstring("not a WAV file"));
    CHECK_THROWS_WITH(fv::parse_wav({}), ContainsSubstring("not a WAV file"));

    // Format 85 is MP3 inside a WAV container.
    CHECK_THROWS_WITH(fv::parse_wav(make_wav(85, 1, 16000, 16, {})),
                      ContainsSubstring("Unsupported WAV encoding"));

    Bytes no_data = make_wav(1, 1, 16000, 16, {});
    no_data.resize(no_data.size() - 8);
    CHECK_THROWS_WITH(fv::parse_wav(no_data), ContainsSubstring("no audio data"));

    CHECK_THROWS_AS(fv::read_wav("no/such/file.wav"), fv::EngineError);
}
