#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace fv::audio {

class DecodedFrame;

namespace detail {
struct ResamplerImpl;
}

// swresample to s16 mono 16 kHz: the decode-window path, as PyAV's
// AudioResampler(format="s16", layout="mono", rate=16000): the default filter, no dither, no
// timestamp compensation, flushed by finish(). Output depends only on the input sample stream,
// not on how it is cut into pushes.
// Affinity: one thread at a time.
class Resampler16k {
public:
    static constexpr int kRate = 16000;

    // Mono int16 input at input_rate (the recorder's PCM).
    explicit Resampler16k(int input_rate);
    // Interleaved int16 input with input_channels channels in FFmpeg's default layout for that
    // count.
    Resampler16k(int input_rate, int input_channels);
    // Decoded frames (push(const DecodedFrame&)); set up from the first frame, as PyAV's
    // resampler is. A later frame of another format, layout or rate throws std::invalid_argument
    // ("Frame does not match AudioResampler setup.").
    Resampler16k();
    ~Resampler16k();
    Resampler16k(Resampler16k&&) noexcept;
    Resampler16k& operator=(Resampler16k&&) noexcept;

    // Appends the 16 kHz samples this input completes to `out`.
    void push(std::span<const std::int16_t>, std::vector<std::int16_t>& out);
    // A frame in the decoder's own sample format, so compressed files are converted exactly
    // once, as PyAV converts them.
    void push(const DecodedFrame&, std::vector<std::int16_t>& out);
    // Flushes the filter's delay. Nothing may be pushed afterwards.
    void finish(std::vector<std::int16_t>& out);

private:
    std::unique_ptr<detail::ResamplerImpl> impl_;
};

} // namespace fv::audio
