#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "meeting_capture/detail/files.h"

// The parts of Python's wave module the spool uses: PCM files with its 44-byte header, and a
// reader with its rules (RIFF and WAVE ids, chunks padded to even sizes, fmt before data, PCM or
// WAVE_FORMAT_EXTENSIBLE with the PCM subformat).
namespace fv::meeting::detail {

// wave.Error, with its messages.
class WaveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The EOFError wave raises for a fmt chunk cut short (it is not a wave.Error).
class WaveEofError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// RIFF, the 16-byte fmt chunk of WAVE_FORMAT_PCM, and the data chunk's header.
std::string wav_header(int rate, int channels, int sample_width, std::uint32_t data_bytes);

// wave.open(path, "wb") for mono 16-bit audio: the header goes first, the sizes are patched on
// close(). A file destroyed without close() keeps what it has.
// Affinity: one thread at a time.
class WavOut {
public:
    WavOut(const std::filesystem::path& path, int rate);
    void write(std::span<const std::int16_t> samples);
    void write(std::span<const std::byte> frames);
    void close();

private:
    BinaryFile file_;
    int rate_;
    std::uint64_t data_bytes_ = 0;
};

struct WavContents {
    int channels = 0;
    int sample_width = 0; // bytes
    int rate = 0;
    std::vector<std::byte> frames; // what readframes(getnframes()) returns
};

// wave.open(path, "rb") and readframes(getnframes()). Throws WaveError, WaveEofError, or
// std::system_error when the file cannot be read.
WavContents read_wav(const std::filesystem::path& path);

// wave.open(path, "rb") kept open, for reading parts of a file: setpos and readframes.
// Affinity: one thread at a time.
class WavReader {
public:
    // Throws as read_wav does.
    explicit WavReader(const std::filesystem::path& path);

    int channels() const { return channels_; }
    int sample_width() const { return sample_width_; } // bytes
    int rate() const { return rate_; }
    // getnframes(): whole frames the data chunk's header announces.
    std::uint64_t frame_count() const { return frame_count_; }
    // setpos(position) then readframes(count): at most `count` frames, fewer where the data
    // ends. Throws WaveError("position not in range") past frame_count().
    std::vector<std::byte> read_frames(std::uint64_t position, std::uint64_t count);

private:
    BinaryFile file_;
    int channels_ = 0;
    int sample_width_ = 0;
    int rate_ = 0;
    std::uint64_t data_offset_ = 0;
    std::uint64_t frame_count_ = 0;
};

// meeting/asr/audio.py load_wav_int16: mono int16 samples and the rate; several channels are
// averaged through int32 and truncated toward zero. Throws py::ValueError ("Expected 16-bit PCM
// WAV, got N-bit: <path>") for another sample width, and what read_wav throws.
std::pair<std::vector<std::int16_t>, int> load_wav_int16(const std::filesystem::path& path);

} // namespace fv::meeting::detail
