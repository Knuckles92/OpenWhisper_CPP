#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace fv::dsp {

// Resamples mono audio to num_samples by zero-padding or truncating its spectrum, with numpy
// 2.x's precisions: rfft in double rounded to complex64, the Nyquist adjustments
// scipy.signal.resample makes, irfft in float, then a float gain of num_samples / samples.size().
// The input is treated as one period of a periodic signal, so callers resample a whole chunk at
// once, never block by block.
//
// Throws std::invalid_argument, numpy's ValueError "Invalid number of FFT data points (0)
// specified.", when samples or num_samples is empty.
//
// Affinity: any thread; no shared state.
std::vector<float> fft_resample(std::span<const float> samples, std::size_t num_samples);

} // namespace fv::dsp
