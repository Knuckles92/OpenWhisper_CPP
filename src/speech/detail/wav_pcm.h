#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace fv::speech::detail {

// Python's wave.Error (and the EOFError of a header cut short), by its message.
class WavError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What wave.open(path, "rb") reports and readframes(getnframes()) returns.
struct WavPcm {
    int channels = 0;
    int sample_width = 0; // bytes
    int rate = 0;
    std::int64_t frames = 0; // getnframes(): the data chunk's size in whole frames
    std::string pcm;         // up to frames * channels * sample_width bytes; fewer when cut short
};

// Reads a RIFF/WAVE as Python 3.12's wave module does: chunks after "WAVE" in order, a PCM or
// extensible-PCM "fmt " first, the first "data" chunk ends the scan, others are skipped (with
// their pad byte). Throws WavError with wave's message ("file does not start with RIFF id",
// "not a WAVE file", "unknown format: 3", "fmt chunk and/or data chunk missing", ...), and
// std::system_error when the file cannot be opened.
// Affinity: any thread; blocks on file I/O.
WavPcm read_wav_pcm(const std::filesystem::path& path);

} // namespace fv::speech::detail
