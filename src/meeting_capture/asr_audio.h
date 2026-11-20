#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

// meeting/asr/audio.py: the audio conversions of meeting ASR. Spooled chunks are 16 kHz mono
// int16 WAVs, but any rate is accepted. Meeting audio is resampled with dsp::fft_resample
// (numpy's FFT resampling, architecture 5.2), never with swresample.
namespace fv::meeting {

// The rate the speech decoder takes (Python's WHISPER_SAMPLE_RATE).
inline constexpr int DECODER_SAMPLE_RATE = 16000;

// prepare_for_whisper: int16 to float32 (divided by 32768), interleaved channels averaged in
// float32, resampled to 16 kHz with round(size * 16000 / src_rate) samples (at least one) when
// src_rate is not 16000, clipped to [-1, 1]. Empty input gives empty output. A rate of 0 throws
// py::PyError("ZeroDivisionError", "division by zero"); a channel count below 1 or one that does
// not divide the samples throws py::ValueError, as numpy's reshape does.
std::vector<float> prepare_for_decoder(std::span<const std::int16_t> frames, int src_rate,
                                       int channels = 1);

// load_wav_int16: a 16-bit PCM WAV's mono samples (several channels averaged through int32 and
// truncated) and its rate. Throws py::ValueError for another sample width, the wave module's
// errors (detail::WaveError) for a malformed file and std::system_error when it cannot be read.
std::pair<std::vector<std::int16_t>, int> load_wav_int16(const std::filesystem::path& path);

} // namespace fv::meeting
