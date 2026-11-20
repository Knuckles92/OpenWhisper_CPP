#include "dsp/fbank.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <numbers>
#include <stdexcept>

// One transform per frame on the caller's thread; the thread pool would only add static state.
#define POCKETFFT_NO_MULTITHREADING
#include <pocketfft_hdronly.h>

namespace fv::dsp {

namespace {

constexpr std::size_t kFrameLength = kFbankFrameLength;
constexpr std::size_t kBins = kFbankBins;
constexpr std::size_t kMelBins = kFbankMelBins;

double mel_scale(double hz) {
    return 1127.0 * std::log(1.0 + hz / 700.0);
}

// numpy.linspace(start, stop, count) in double: i * step + start, the last point exactly stop.
std::vector<double> linspace(double start, double stop, std::size_t count) {
    std::vector<double> points(count);
    const double step = (stop - start) / static_cast<double>(count - 1);
    for (std::size_t i = 0; i < count; ++i) {
        points[i] = static_cast<double>(i) * step + start;
    }
    points.back() = stop;
    return points;
}

// Triangles built in the mel domain (Kaldi's convention): filter i rises from edge i to centre
// i + 1 and falls to edge i + 2 of kFbankMelBins + 2 equally spaced mel points.
std::vector<float> build_mel_filters() {
    const std::vector<double> fft_freqs = linspace(0.0, kFbankSampleRate / 2.0, kBins);
    std::vector<double> mel_freqs(kBins);
    for (std::size_t k = 0; k < kBins; ++k) {
        mel_freqs[k] = mel_scale(fft_freqs[k]);
    }
    const std::vector<double> mel_points =
        linspace(mel_scale(kFbankLowFreq), mel_scale(kFbankHighFreq), kMelBins + 2);
    std::vector<float> filters(kMelBins * kBins);
    for (std::size_t i = 0; i < kMelBins; ++i) {
        const double left = mel_points[i];
        const double center = mel_points[i + 1];
        const double right = mel_points[i + 2];
        for (std::size_t k = 0; k < kBins; ++k) {
            const double up = (mel_freqs[k] - left) / (center - left);
            const double down = (right - mel_freqs[k]) / (right - center);
            filters[i * kBins + k] = static_cast<float>(std::max(0.0, std::min(up, down)));
        }
    }
    return filters;
}

// The Povey window: (0.5 - 0.5 cos(2 pi n / (N - 1)))^0.85.
std::vector<float> build_window() {
    std::vector<float> window(kFrameLength);
    for (std::size_t n = 0; n < kFrameLength; ++n) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                                 static_cast<double>(kFrameLength - 1));
        window[n] = static_cast<float>(std::pow(hann, 0.85));
    }
    return window;
}

void require_rate(int rate) {
    if (rate != kFbankSampleRate) {
        throw std::invalid_argument(
            std::format("Filterbank features need 16000 Hz audio, not {} Hz.", rate));
    }
}

// `scaled` holds the samples in the int16 range, in double, as Python's frames are.
Fbank features(const std::vector<double>& scaled) {
    Fbank out;
    if (scaled.size() < kFrameLength) {
        return out;
    }
    const std::vector<float>& filters = fbank_mel_filters();
    const std::vector<float>& window = fbank_window();
    const std::size_t frames = (scaled.size() - kFrameLength) / kFbankFrameShift + 1;
    out.frames = static_cast<int>(frames);
    out.values.resize(frames * kMelBins);

    const pocketfft::detail::pocketfft_r<double> plan(kFbankFftSize);
    std::vector<double> frame(kFrameLength);
    // pocketfft's packed layout r0, r1, i1, ..., r256, written one slot in and then r0 moved to
    // slot 0, so that bin k sits at 2k (real) and 2k + 1 (imaginary). The value-initialised tail
    // is the zero padding to 512 points and the Nyquist bin's zero imaginary part.
    std::vector<double> packed(2 * kBins);
    std::vector<double> power(kBins);
    for (std::size_t t = 0; t < frames; ++t) {
        const double* source = scaled.data() + t * kFbankFrameShift;
        double sum = 0.0;
        for (std::size_t n = 0; n < kFrameLength; ++n) {
            sum += source[n];
        }
        const double mean = sum / static_cast<double>(kFrameLength);
        for (std::size_t n = 0; n < kFrameLength; ++n) {
            frame[n] = source[n] - mean;
        }
        // Pre-emphasis from the DC-removed frame, the first sample paired with itself.
        std::fill(packed.begin(), packed.end(), 0.0);
        for (std::size_t n = 0; n < kFrameLength; ++n) {
            const double previous = frame[n == 0 ? 0 : n - 1];
            packed[1 + n] =
                (frame[n] - kFbankPreemphasis * previous) * static_cast<double>(window[n]);
        }
        plan.exec(packed.data() + 1, 1.0, pocketfft::FORWARD);
        packed[0] = packed[1];
        packed[1] = 0.0;
        for (std::size_t k = 0; k < kBins; ++k) {
            const double re = packed[2 * k];
            const double im = packed[2 * k + 1];
            power[k] = re * re + im * im;
        }
        float* row = out.values.data() + t * kMelBins;
        for (std::size_t i = 0; i < kMelBins; ++i) {
            const float* weights = filters.data() + i * kBins;
            double energy = 0.0;
            for (std::size_t k = 0; k < kBins; ++k) {
                energy += power[k] * static_cast<double>(weights[k]);
            }
            row[i] = static_cast<float>(std::log(std::max(energy, kFbankLogFloor)));
        }
    }
    return out;
}

} // namespace

const std::vector<float>& fbank_mel_filters() {
    static const std::vector<float> filters = build_mel_filters();
    return filters;
}

const std::vector<float>& fbank_window() {
    static const std::vector<float> window = build_window();
    return window;
}

Fbank compute_fbank(std::span<const float> audio, int rate) {
    require_rate(rate);
    std::vector<double> scaled(audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i) {
        scaled[i] = static_cast<double>(audio[i]) * kFbankInt16Scale;
    }
    return features(scaled);
}

Fbank compute_fbank(std::span<const std::int16_t> audio, int rate) {
    require_rate(rate);
    // Python first normalises int16 to float32 (value / 32768, exact) and scales back up, which
    // gives the integer value again.
    std::vector<double> scaled(audio.size());
    for (std::size_t i = 0; i < audio.size(); ++i) {
        scaled[i] = static_cast<double>(static_cast<float>(audio[i]) / 32768.0f) * kFbankInt16Scale;
    }
    return features(scaled);
}

void apply_cmn(Fbank& features) {
    if (features.frames <= 0) {
        return;
    }
    // numpy's float32 mean over axis 0 adds the rows one after another in float, then divides
    // by the count in float.
    std::vector<float> mean(kMelBins, 0.0f);
    for (int t = 0; t < features.frames; ++t) {
        const float* row = features.values.data() + static_cast<std::size_t>(t) * kMelBins;
        for (std::size_t i = 0; i < kMelBins; ++i) {
            mean[i] += row[i];
        }
    }
    const auto count = static_cast<float>(features.frames);
    for (float& value : mean) {
        value /= count;
    }
    for (int t = 0; t < features.frames; ++t) {
        float* row = features.values.data() + static_cast<std::size_t>(t) * kMelBins;
        for (std::size_t i = 0; i < kMelBins; ++i) {
            row[i] -= mean[i];
        }
    }
}

} // namespace fv::dsp
