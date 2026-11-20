#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>

#include "walkthrough/sections.h"

namespace fv::walkthrough::detail {

// What Python's wave.open(path, "rb") reads: the format, and the data chunk's bytes as far as
// the chunk, the RIFF chunk and the file all reach.
struct WaveFile {
    int channels = 0;
    int sample_width = 0; // bytes
    int rate = 0;
    std::string pcm; // little-endian frames, whole frames only
};

// Throws WaveError, WaveTruncated or std::filesystem::filesystem_error.
WaveFile read_wave(const std::filesystem::path& path);

// The frames [start, end) of each span, joined, as a 16-bit WAV with Python's wave header.
// Throws std::system_error.
void write_pcm16_wav(const std::filesystem::path& path, const Pcm16& audio,
                     std::span<const std::pair<std::int64_t, std::int64_t>> spans);

} // namespace fv::walkthrough::detail
