// Ported from tests/test_meeting_diarize.py::TestFbank, plus the golden vectors of
// meeting.diarize.fbank.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dsp/fbank.h"
#include "golden/golden.h"

using fv::dsp::apply_cmn;
using fv::dsp::compute_fbank;
using fv::dsp::Fbank;
using fv::dsp::kFbankBins;
using fv::dsp::kFbankFftSize;
using fv::dsp::kFbankFrameLength;
using fv::dsp::kFbankFrameShift;
using fv::dsp::kFbankHighFreq;
using fv::dsp::kFbankMelBins;
using fv::dsp::kFbankSampleRate;

namespace {

// TestFbank._tone: 0.5 sin(2 pi f t) at 16 kHz, computed in double and stored as float32.
std::vector<float> tone(double freq, double seconds = 1.0) {
    const auto count = static_cast<std::size_t>(kFbankSampleRate * seconds);
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * freq *
                                                   static_cast<double>(i) / kFbankSampleRate));
    }
    return out;
}

double column_mean(const Fbank& features, int bin) {
    double sum = 0.0;
    for (int t = 0; t < features.frames; ++t) {
        sum += features.row(t)[static_cast<std::size_t>(bin)];
    }
    return sum / features.frames;
}

std::vector<std::int16_t> pcm_from_base64(std::string_view text) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const auto index = alphabet.find(c);
        if (c == '=' || index == std::string_view::npos) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    std::vector<std::int16_t> pcm(bytes.size() / 2);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(bytes[2 * i]) |
                                           (static_cast<std::uint16_t>(bytes[2 * i + 1]) << 8));
    }
    return pcm;
}

void check_values(const std::vector<float>& actual, const fv::Json& expected, double tolerance,
                  const std::string& what) {
    REQUIRE(actual.size() == expected.size());
    std::size_t worst = 0;
    double worst_delta = 0.0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const double delta =
            std::abs(static_cast<double>(actual[i]) - fv::golden::decode<double>(expected[i]));
        if (delta > worst_delta) {
            worst_delta = delta;
            worst = i;
        }
    }
    INFO(what << ": largest difference " << worst_delta << " at value " << worst);
    CHECK(worst_delta <= tolerance);
}

} // namespace

// from tests/test_meeting_diarize.py::TestFbank
TEST_CASE("TestFbank::test_shape_and_determinism", "[test_meeting_diarize]") {
    const std::vector<float> audio = tone(440.0, 0.5);
    const Fbank features = compute_fbank(audio);
    const auto expected_frames =
        static_cast<int>((audio.size() - kFbankFrameLength) / kFbankFrameShift + 1);
    CHECK(features.frames == expected_frames);
    CHECK(features.values.size() == static_cast<std::size_t>(expected_frames) * kFbankMelBins);
    CHECK(std::all_of(features.values.begin(), features.values.end(),
                      [](float value) { return std::isfinite(value); }));
    CHECK(compute_fbank(audio).values == features.values);
}

TEST_CASE("TestFbank::test_short_audio_yields_no_frames", "[test_meeting_diarize]") {
    const std::vector<float> audio(100, 0.0f);
    const Fbank features = compute_fbank(audio);
    CHECK(features.frames == 0);
    CHECK(features.values.empty());
}

TEST_CASE("TestFbank::test_int16_input_matches_normalized_float", "[test_meeting_diarize]") {
    const std::vector<float> audio = tone(300.0, 0.2);
    // (audio * 32768.0).astype(np.int16): truncation toward zero (0.5 * 32768 fits).
    std::vector<std::int16_t> as_int16(audio.size());
    std::vector<float> normalized(audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i) {
        as_int16[i] = static_cast<std::int16_t>(audio[i] * 32768.0f);
        normalized[i] = static_cast<float>(as_int16[i]) / 32768.0f;
    }
    const Fbank from_int16 = compute_fbank(as_int16);
    const Fbank from_float = compute_fbank(normalized);
    REQUIRE(from_int16.frames == from_float.frames);
    for (std::size_t i = 0; i < from_int16.values.size(); ++i) {
        const double expected = from_float.values[i];
        CHECK(std::abs(from_int16.values[i] - expected) <= 1e-4 + 1e-4 * std::abs(expected));
    }
}

TEST_CASE("TestFbank::test_cmn_zeroes_the_time_mean", "[test_meeting_diarize]") {
    Fbank features = compute_fbank(tone(500.0, 0.3));
    apply_cmn(features);
    for (int bin = 0; bin < kFbankMelBins; ++bin) {
        CHECK(std::abs(column_mean(features, bin)) <= 1e-3);
    }
    Fbank empty;
    apply_cmn(empty);
    CHECK(empty.frames == 0);
}

TEST_CASE("TestFbank::test_filterbank_reaches_nyquist", "[test_meeting_diarize]") {
    // Kaldi, torchaudio and WeSpeaker take high_freq = Nyquist. A narrower band (the old
    // 7600 Hz) shifts every mel bin away from the model's training features.
    STATIC_CHECK(kFbankHighFreq == kFbankSampleRate / 2.0);
    const std::vector<float>& filters = fv::dsp::fbank_mel_filters();
    REQUIRE(filters.size() == static_cast<std::size_t>(kFbankMelBins) * kFbankBins);
    STATIC_CHECK(kFbankBins == kFbankFftSize / 2 + 1);

    std::vector<double> coverage(kFbankBins, 0.0);
    for (int i = 0; i < kFbankMelBins; ++i) {
        for (int k = 0; k < kFbankBins; ++k) {
            coverage[static_cast<std::size_t>(k)] +=
                filters[static_cast<std::size_t>(i) * kFbankBins + static_cast<std::size_t>(k)];
        }
    }
    double above_7600 = 0.0;
    for (int k = 0; k < kFbankBins; ++k) {
        const double freq = k * (kFbankSampleRate / 2.0) / (kFbankBins - 1);
        if (freq > 7600.0) {
            above_7600 = std::max(above_7600, coverage[static_cast<std::size_t>(k)]);
        }
    }
    CHECK(above_7600 > 0.0);
    // The Nyquist bin sits exactly on the last triangle's right edge.
    CHECK(std::abs(coverage.back()) <= 1e-6);
    CHECK(coverage[coverage.size() - 2] > 0.0);
    CHECK(*std::min_element(filters.begin(), filters.end()) >= 0.0f);
}

TEST_CASE("TestFbank::test_top_mel_bin_responds_to_a_7800_hz_tone", "[test_meeting_diarize]") {
    // The functional companion of the band-edge guard above: near-Nyquist energy lands in the
    // top mel bin rather than being discarded.
    const Fbank top_tone = compute_fbank(tone(7800.0, 0.2));
    const std::vector<float> zeros(kFbankSampleRate / 5, 0.0f);
    const Fbank silence = compute_fbank(zeros);
    CHECK(column_mean(top_tone, kFbankMelBins - 1) >
          column_mean(silence, kFbankMelBins - 1) + 10.0);
}

TEST_CASE("compute_fbank refuses a rate other than 16 kHz", "[fbank]") {
    const std::vector<float> audio(800, 0.0f);
    CHECK_THROWS_AS(compute_fbank(audio, 48000), std::invalid_argument);
}

TEST_CASE("compute_fbank and apply_cmn replay the golden vectors", "[golden][fbank]") {
    fv::golden::for_each("meeting.diarize.fbank", "compute_fbank", [](const fv::golden::Case& c) {
        const fv::Json& expected = c.raw().at("result");
        const std::vector<std::int16_t> pcm =
            pcm_from_base64(expected.at("pcm").get<std::string>());
        Fbank features;
        if (c.arg<std::string>(1) == "int16") {
            features = compute_fbank(pcm);
        } else {
            std::vector<float> audio(pcm.size());
            for (std::size_t i = 0; i < pcm.size(); ++i) {
                audio[i] = static_cast<float>(pcm[i]) / 32768.0f;
            }
            features = compute_fbank(audio);
        }
        REQUIRE(features.frames == expected.at("frames").get<int>());
        check_values(features.values, expected.at("fbank"), c.float_tolerance(), "fbank");
        apply_cmn(features);
        check_values(features.values, expected.at("cmn"), c.float_tolerance(), "cmn");
    });
}

TEST_CASE("the filterbank and the window replay the golden vectors", "[golden][fbank]") {
    fv::golden::for_each("meeting.diarize.fbank", "mel_filters", [](const fv::golden::Case& c) {
        const fv::Json& expected = c.raw().at("result");
        const std::vector<float>& filters = fv::dsp::fbank_mel_filters();
        REQUIRE(expected.size() == static_cast<std::size_t>(kFbankMelBins));
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const float* row = filters.data() + i * kFbankBins;
            int first = -1;
            int last = -1;
            float sum = 0.0f;
            for (int k = 0; k < kFbankBins; ++k) {
                if (row[k] != 0.0f) {
                    first = first < 0 ? k : first;
                    last = k;
                }
                sum += row[k];
            }
            INFO("filter " << i);
            CHECK(first == expected[i][0].get<int>());
            CHECK(last == expected[i][1].get<int>());
            CHECK(std::abs(sum - fv::golden::decode<double>(expected[i][2])) <=
                  c.float_tolerance());
        }
    });
    fv::golden::for_each("meeting.diarize.fbank", "window", [](const fv::golden::Case& c) {
        check_values(fv::dsp::fbank_window(), c.raw().at("result"), 1e-7, "window");
    });
}
