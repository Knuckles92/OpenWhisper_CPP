// Ported from tests/test_resample.py, plus the golden vectors of services.streaming_transcriber.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#define POCKETFFT_NO_MULTITHREADING
#include <pocketfft_hdronly.h>

#include "dsp/fft_resample.h"
#include "golden/golden.h"
#include "signals.h"

using Catch::Matchers::WithinAbs;
using fv::dsp::fft_resample;

namespace {

// config.SAMPLE_RATE and config.WHISPER_TARGET_SAMPLE_RATE.
constexpr int kRecorderRate = 44100;
constexpr int kWhisperRate = 16000;

std::vector<float> sine(double freq_hz, int sample_rate, double duration_s) {
    const auto count = static_cast<std::size_t>(sample_rate * duration_s);
    std::vector<float> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        out[i] = static_cast<float>(std::sin(2 * std::numbers::pi * freq_hz * t));
    }
    return out;
}

// The rfftfreq of the largest |rfft| bin.
double dominant_freq(const std::vector<float>& samples, int sample_rate) {
    const std::vector<double> input(samples.begin(), samples.end());
    std::vector<std::complex<double>> spectrum(input.size() / 2 + 1);
    pocketfft::r2c<double>({input.size()}, {sizeof(double)}, {sizeof(std::complex<double>)}, 0,
                           pocketfft::FORWARD, input.data(), spectrum.data(), 1.0);
    std::size_t best = 0;
    for (std::size_t k = 1; k < spectrum.size(); ++k) {
        if (std::abs(spectrum[k]) > std::abs(spectrum[best])) {
            best = k;
        }
    }
    return static_cast<double>(best) * sample_rate / static_cast<double>(samples.size());
}

} // namespace

TEST_CASE("fft_resample replays the golden vectors", "[golden][fft_resample]") {
    fv::golden::for_each(
        "services.streaming_transcriber", "fft_resample", [](const fv::golden::Case& c) {
            const auto input = fv::dsp::test::resample_signal(
                c.arg<std::string>(0), c.arg<std::size_t>(1), c.arg<std::uint64_t>(3));
            const auto num_samples = c.arg<std::size_t>(2);
            if (c.raises()) {
                CHECK(c.raises_type() == "ValueError");
                CHECK_THROWS_AS(fft_resample(input, num_samples), std::invalid_argument);
                return;
            }
            const auto expected = c.result<std::vector<float>>();
            const std::vector<float> got = fft_resample(input, num_samples);
            REQUIRE(got.size() == expected.size());
            // Counted per sample so a NaN fails: std::max(worst, NaN) would keep worst.
            std::size_t outside = 0;
            for (std::size_t i = 0; i < got.size(); ++i) {
                const double difference = std::abs(static_cast<double>(got[i]) - expected[i]);
                if (!(difference <= c.float_tolerance())) {
                    ++outside;
                }
            }
            CHECK(outside == 0);
        });
}

TEST_CASE("an empty input or output is numpy's ValueError", "[fft_resample]") {
    const std::vector<float> none;
    const std::vector<float> some{0.1f, 0.2f, 0.3f};
    CHECK_THROWS_WITH(fft_resample(none, 4), "Invalid number of FFT data points (0) specified.");
    CHECK_THROWS_WITH(fft_resample(some, 0), "Invalid number of FFT data points (0) specified.");
}

// from tests/test_resample.py::test_output_length_and_dtype
TEST_CASE("output length and dtype", "[test_resample]") {
    const auto out = fft_resample(sine(440, 44100, 1.0), 16000);
    CHECK(out.size() == 16000);
    STATIC_CHECK(std::is_same_v<decltype(out)::value_type, float>);
}

// from tests/test_resample.py::test_downsample_preserves_tone
TEST_CASE("downsample preserves tone", "[test_resample]") {
    const double freq_hz = GENERATE(220.0, 1000.0, 3000.0);
    const auto signal = sine(freq_hz, kRecorderRate, 1.0);

    const auto out = fft_resample(signal, kWhisperRate);

    CHECK_THAT(dominant_freq(out, kWhisperRate), WithinAbs(freq_hz, 2.0));
}

// from tests/test_resample.py::test_downsample_preserves_amplitude
TEST_CASE("downsample preserves amplitude", "[test_resample]") {
    const auto out = fft_resample(sine(1000, 44100, 1.0), 16000);
    // A unit sine has RMS 1/sqrt(2); resampling must not rescale it.
    double sum = 0.0;
    for (const float value : out) {
        sum += static_cast<double>(value) * value;
    }
    CHECK_THAT(std::sqrt(sum / static_cast<double>(out.size())), WithinAbs(0.7071, 0.01));
}

// from tests/test_resample.py::test_downsample_attenuates_above_nyquist
TEST_CASE("downsample attenuates above nyquist", "[test_resample]") {
    // 12 kHz is above the 8 kHz Nyquist of the 16 kHz output. Naive decimation would fold it
    // back to an audible 4 kHz tone; FFT resampling drops it.
    const auto out = fft_resample(sine(12000, 44100, 1.0), kWhisperRate);

    float peak = 0.0f;
    for (const float value : out) {
        peak = std::max(peak, std::abs(value));
    }
    CHECK(peak < 0.05f);
}

// from tests/test_resample.py::test_identity_length_roundtrips
TEST_CASE("identity length roundtrips", "[test_resample]") {
    const auto signal = sine(440, 16000, 0.5);
    const auto out = fft_resample(signal, signal.size());
    REQUIRE(out.size() == signal.size());
    // np.allclose(out, signal, atol=1e-5): |a - b| <= atol + rtol * |b| with rtol 1e-5.
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double difference = std::abs(static_cast<double>(out[i]) - signal[i]);
        CHECK(difference <= 1e-5 + 1e-5 * std::abs(static_cast<double>(signal[i])));
    }
}

// from tests/test_resample.py::test_upsample_preserves_tone
TEST_CASE("upsample preserves tone", "[test_resample]") {
    const double duration_s = 0.5;
    const int dst_rate = 22050;
    const auto out =
        fft_resample(sine(440, 16000, duration_s), static_cast<std::size_t>(dst_rate * duration_s));
    CHECK_THAT(dominant_freq(out, dst_rate), WithinAbs(440.0, 3.0));
}

// from tests/test_resample.py::test_short_and_odd_lengths_do_not_raise
TEST_CASE("short and odd lengths do not raise", "[test_resample]") {
    const std::size_t num_samples = GENERATE(1u, 2u, 3u, 999u, 1000u, 1001u);
    const auto out = fft_resample(sine(440, 44100, 0.05), num_samples);
    CHECK(out.size() == num_samples);
    CHECK(std::all_of(out.begin(), out.end(), [](float value) { return std::isfinite(value); }));
}
