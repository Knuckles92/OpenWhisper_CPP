#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace fv::audio {

namespace detail {
struct WavWriterImpl;
}

// PCM WAV saved atomically, as services/recorder.py save_recording writes it: a temporary
// "tmp*.wav" from tempfile.mkstemp in the target's folder, Python's wave header (RIFF, a
// 16-byte fmt of WAVE_FORMAT_PCM, data), the frames, the origination tags (a failure costs
// only the tags: "Failed to stamp WAV origination metadata" is logged), fsync, then os.replace
// over the target. Until commit() the previous target is untouched; a writer destroyed without
// commit() removes its temporary file.
// Affinity: one thread at a time; blocks on disk I/O, never on the GUI thread.
class WavWriter {
public:
    struct Format {
        int rate = 44100;
        int channels = 1;
        int sample_width = 2; // bytes per sample
    };

    // Throws std::system_error when the temporary file cannot be created.
    WavWriter(std::filesystem::path target, Format format);
    ~WavWriter();
    WavWriter(WavWriter&&) noexcept;
    WavWriter& operator=(WavWriter&&) noexcept;

    // Raw little-endian frames (wave's writeframesraw). Throws std::system_error.
    void write(std::span<const std::byte> frames);
    void write(std::span<const std::int16_t> samples);
    // Data bytes written so far.
    std::uint64_t data_bytes() const;
    const std::filesystem::path& temporary_path() const;

    // Sizes in the header, tags stamped with `origination` (local wall time; none: no tags),
    // flushed to disk and moved over the target. Throws std::system_error; the temporary file is
    // removed and the target left as it was.
    void commit(std::optional<std::chrono::local_seconds> origination);
    // Removes the temporary file; the target is left as it was.
    void discard();

private:
    std::unique_ptr<detail::WavWriterImpl> impl_;
};

} // namespace fv::audio
