#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// RIFF/WAVE origination tags for File Explorer and other media browsers (services/
// wav_metadata.py). Windows Details columns such as Media created read LIST INFO ICRD and
// Broadcast Wave bext dates, so each saved recording carries both. The software and originator
// are identity().product.
namespace fv::audio {

inline constexpr std::size_t kBextSize = 602;
// RIFF stores its size as an unsigned 32-bit field.
inline constexpr std::uint64_t kMaxRiffSize = 0xFFFFFFFFu;

// The file is not a RIFF WAVE that can be tagged (Python's WavMetadataError, a ValueError).
class WavMetadataError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A file opened for reading and writing, at the level of its descriptor: a write that fails is
// not retried behind the caller's back, so a partial tag can be taken back.
// Affinity: one thread at a time.
class RiffFile {
public:
    virtual ~RiffFile() = default;
    virtual std::uint64_t seek_end() = 0; // moves to the end; returns the size in bytes
    virtual void seek(std::uint64_t offset) = 0;
    // Writes at the position; returns the bytes written (0 when nothing more fits). Throws
    // std::system_error.
    virtual std::size_t write(std::span<const std::byte> data) = 0;
    virtual void truncate(std::uint64_t size) = 0;
};

// An existing file opened for reading and writing. Throws std::system_error.
std::unique_ptr<RiffFile> open_riff_file(const std::filesystem::path& path);

// Tags the RIFF/WAVE just written to `wav`, in place: LIST INFO (ICRD "YYYY-MM-DD", ISFT) and
// a 602-byte bext (originator, date, time) after its last chunk, then the RIFF size. An
// odd-length file gets its pad byte first. `when` is local wall time. A RIFF that would exceed
// max_riff_size throws WavMetadataError("Recording is too large for RIFF metadata") and writes
// nothing; any other failure is rethrown after the file is put back as the untagged WAV it was.
void append_origination(RiffFile& wav, std::chrono::local_seconds when,
                        std::uint64_t max_riff_size = kMaxRiffSize);

// Whether `path` is a RIFF/WAVE of 16-bit integer PCM with one channel (what Quick Record
// saves). Reads only the chunk headers before data (at most 16); a file that cannot be read,
// or is not a WAV, is simply not one.
bool is_mono_pcm16(const std::filesystem::path& path);

// The LIST INFO creation date and software tag, or nullopt. Throw std::system_error when the
// file cannot be read and WavMetadataError for non-ASCII text.
std::optional<std::string> read_info_icrd(const std::filesystem::path& path);
std::optional<std::string> read_info_isft(const std::filesystem::path& path);
// (YYYY-MM-DD, HH:MM:SS) from bext, or nullopt.
std::optional<std::pair<std::string, std::string>>
read_bext_origination(const std::filesystem::path& path);

} // namespace fv::audio
