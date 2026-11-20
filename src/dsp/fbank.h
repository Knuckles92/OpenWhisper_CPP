#pragma once

#include <cstdint>
#include <span>
#include <vector>

// Kaldi-style 80-bin log-mel filterbank features, the front end of the speaker-embedding model
// (WeSpeaker family), matching Kaldi's compute-fbank-feats defaults: 25 ms frames every 10 ms
// with snip-edges framing, per-frame DC removal, pre-emphasis 0.97 (the first sample paired with
// itself), the Povey window (Hann^0.85), a 512-point FFT, the power spectrum, 80 triangular bins
// on Kaldi's mel scale (1127 ln(1 + f/700)) from 20 Hz to Nyquist, and a natural log floored at
// 1e-10. No dither: the features are deterministic.
namespace fv::dsp {

inline constexpr int kFbankSampleRate = 16000;
inline constexpr int kFbankFrameLength = 400; // 25 ms at 16 kHz
inline constexpr int kFbankFrameShift = 160;  // 10 ms at 16 kHz
inline constexpr int kFbankFftSize = 512;     // the next power of two >= the frame length
inline constexpr int kFbankBins = kFbankFftSize / 2 + 1;
inline constexpr int kFbankMelBins = 80;
inline constexpr double kFbankLowFreq = 20.0;
// Nyquist, Kaldi's and torchaudio's high_freq=0, which WeSpeaker trains with. 7600 Hz (an HTK
// convention) would shift every mel bin away from the model's training features. The Nyquist
// FFT bin still gets zero weight: it sits exactly on the last triangle's right edge.
inline constexpr double kFbankHighFreq = kFbankSampleRate / 2.0;
inline constexpr double kFbankPreemphasis = 0.97;
inline constexpr double kFbankLogFloor = 1e-10;
// Kaldi and torchaudio work on int16-range samples. The gain only matters near the log floor;
// cepstral mean normalisation removes any constant offset.
inline constexpr double kFbankInt16Scale = 32768.0;

// Features of `frames` rows of kFbankMelBins values, row-major.
struct Fbank {
    int frames = 0;
    std::vector<float> values;

    std::span<const float> row(int frame) const {
        return std::span<const float>(values).subspan(
            static_cast<std::size_t>(frame) * kFbankMelBins, kFbankMelBins);
    }
};

// Features of mono audio at 16 kHz, nominally in [-1, 1]; no frames when the audio is shorter
// than one 25 ms frame. The int16 overload reads each sample as value / 32768, as Python does.
// Only 16 kHz exists: another rate throws std::invalid_argument.
// Affinity: any thread.
Fbank compute_fbank(std::span<const float> audio, int rate = kFbankSampleRate);
Fbank compute_fbank(std::span<const std::int16_t> audio, int rate = kFbankSampleRate);

// Cepstral mean normalisation: subtracts each bin's mean over time, in float as numpy does on
// float32 features. No frames stay no frames.
void apply_cmn(Fbank& features);

// The filterbank, kFbankMelBins rows of kFbankBins weights, and the Povey window of
// kFbankFrameLength values, both rounded to float as Python keeps them.
const std::vector<float>& fbank_mel_filters();
const std::vector<float>& fbank_window();

} // namespace fv::dsp
