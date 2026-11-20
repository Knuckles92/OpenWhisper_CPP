#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace fv {

// Mono float samples in [-1, 1].
struct AudioBuffer {
    std::vector<float> samples;
    int sample_rate = 0;

    double duration() const {
        return sample_rate > 0 ? static_cast<double>(samples.size()) / sample_rate : 0.0;
    }
};

// Decodes a RIFF/WAVE file: integer PCM of 8, 16, 24 or 32 bits, or 32- or
// 64-bit float, with any channel count (channels are averaged to mono). The
// sample rate is kept as it is. Throws EngineError for anything else.
AudioBuffer parse_wav(std::span<const std::uint8_t> bytes);
AudioBuffer read_wav(const std::filesystem::path& path);

} // namespace fv
