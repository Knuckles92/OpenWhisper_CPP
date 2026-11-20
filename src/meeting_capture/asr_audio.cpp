#include "meeting_capture/asr_audio.h"

#include <algorithm>
#include <format>

#include "dsp/fft_resample.h"
#include "meeting_capture/detail/wav_file.h"
#include "pyport/errors.h"
#include "pyport/number.h"

namespace fv::meeting {

std::vector<float> prepare_for_decoder(std::span<const std::int16_t> frames, int src_rate,
                                       int channels) {
    if (channels < 1 || frames.size() % static_cast<std::size_t>(channels) != 0) {
        throw py::ValueError(std::format("cannot reshape array of size {} into shape (-1,{})",
                                         frames.size(), channels));
    }
    // numpy: float32(x) / 32768.0 stays in float32, and dividing by a power of two is exact.
    const auto width = static_cast<std::size_t>(channels);
    std::vector<float> audio(frames.size() / width);
    for (std::size_t i = 0; i < audio.size(); ++i) {
        if (width == 1) {
            audio[i] = static_cast<float>(frames[i]) / 32768.0f;
            continue;
        }
        float sum = 0.0f;
        for (std::size_t c = 0; c < width; ++c) {
            sum += static_cast<float>(frames[i * width + c]) / 32768.0f;
        }
        audio[i] = sum / static_cast<float>(channels);
    }
    if (audio.empty()) {
        return audio;
    }
    if (src_rate != DECODER_SAMPLE_RATE) {
        if (src_rate == 0) {
            // audio.size * 16000 / src_rate divides two Python ints.
            throw py::PyError("ZeroDivisionError", "division by zero");
        }
        const double exact =
            static_cast<double>(audio.size()) * DECODER_SAMPLE_RATE / static_cast<double>(src_rate);
        const auto count =
            std::max<std::int64_t>(1, static_cast<std::int64_t>(py::round_half_even(exact)));
        audio = dsp::fft_resample(audio, static_cast<std::size_t>(count));
    }
    for (float& sample : audio) {
        sample = std::clamp(sample, -1.0f, 1.0f);
    }
    return audio;
}

std::pair<std::vector<std::int16_t>, int> load_wav_int16(const std::filesystem::path& path) {
    return detail::load_wav_int16(path);
}

} // namespace fv::meeting
