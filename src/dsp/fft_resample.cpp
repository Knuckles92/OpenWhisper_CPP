#include "dsp/fft_resample.h"

#include <algorithm>
#include <complex>
#include <format>
#include <stdexcept>

// One transform per call on the caller's thread; the thread pool would only add static state.
#define POCKETFFT_NO_MULTITHREADING
#include <pocketfft_hdronly.h>

namespace fv::dsp {

namespace {

[[noreturn]] void invalid_points(std::size_t count) {
    throw std::invalid_argument(
        std::format("Invalid number of FFT data points ({}) specified.", count));
}

// numpy.fft.rfft on float32 input. Its output is complex64, but numpy passes the normalisation
// factor as the Python int 1, which selects the float64 loop: the transform runs in double and
// only the result is rounded to complex64 (measured against numpy 2.5.2, every length 1-399).
std::vector<std::complex<float>> rfft(std::span<const float> samples) {
    const std::size_t count = samples.size();
    const std::size_t bins = count / 2 + 1;
    // pocketfft works in place on the packed layout r0, r1, i1, r2, i2, ...; written one value
    // in, moving r0 to slot 0 afterwards leaves every bin in its complex slot, which is how
    // numpy unpacks it. The value-initialised tail is the zero imaginary part of an even
    // length's Nyquist bin.
    std::vector<double> packed(2 * bins, 0.0);
    std::copy(samples.begin(), samples.end(), packed.begin() + 1);
    const pocketfft::detail::pocketfft_r<double> plan(count);
    plan.exec(packed.data() + 1, 1.0, pocketfft::FORWARD);
    packed[0] = packed[1];
    packed[1] = 0.0;
    std::vector<std::complex<float>> spectrum(bins);
    for (std::size_t i = 0; i < bins; ++i) {
        spectrum[i] = {static_cast<float>(packed[2 * i]), static_cast<float>(packed[2 * i + 1])};
    }
    return spectrum;
}

// numpy.fft.irfft(spectrum, count) on complex64, in single precision (its normalisation factor
// is a float32 scalar, which selects the float32 loop): the imaginary parts of the DC bin and of an
// even length's Nyquist bin are dropped, missing bins are zero, bins past count / 2 are ignored,
// and the 1 / count normalisation is a float32 reciprocal applied inside the transform.
std::vector<float> irfft(std::span<const std::complex<float>> spectrum, std::size_t count) {
    std::vector<float> out(count);
    out[0] = spectrum[0].real();
    if (count > 1) {
        const std::size_t pairs = std::min(spectrum.size() - 1, (count - 1) / 2);
        for (std::size_t i = 0; i < pairs; ++i) {
            out[1 + 2 * i] = spectrum[1 + i].real();
            out[2 + 2 * i] = spectrum[1 + i].imag();
        }
        if (count % 2 == 0 && count / 2 < spectrum.size()) {
            out[count - 1] = spectrum[count / 2].real();
        }
    }
    const float scale = 1.0f / static_cast<float>(count);
    const pocketfft::detail::pocketfft_r<float> plan(count);
    plan.exec(out.data(), scale, pocketfft::BACKWARD);
    return out;
}

} // namespace

std::vector<float> fft_resample(std::span<const float> samples, std::size_t num_samples) {
    const std::size_t n_in = samples.size();
    if (n_in == 0 || num_samples == 0) {
        invalid_points(0);
    }
    std::vector<std::complex<float>> spectrum = rfft(samples);
    const std::size_t n_out_bins = num_samples / 2 + 1;

    // irfft counts the last bin once (its own conjugate twin) and every other bin twice, so the
    // Nyquist bin needs care whenever the length changes; scipy.signal.resample makes the same
    // two adjustments.
    if (num_samples < n_in) {
        spectrum.resize(n_out_bins);
        if (num_samples % 2 == 0) {
            // Truncation moved an ordinary two-sided bin into the Nyquist slot.
            spectrum.back() *= 2.0f;
        }
    } else if (num_samples > n_in) {
        if (n_in % 2 == 0) {
            // The input's real Nyquist bin becomes an ordinary two-sided one.
            spectrum.back() *= 0.5f;
        }
        spectrum.resize(n_out_bins);
    }

    std::vector<float> out = irfft(spectrum, num_samples);
    // numpy multiplies the float32 array by the Python float ratio cast to float32.
    const auto gain =
        static_cast<float>(static_cast<double>(num_samples) / static_cast<double>(n_in));
    for (float& value : out) {
        value *= gain;
    }
    return out;
}

} // namespace fv::dsp
