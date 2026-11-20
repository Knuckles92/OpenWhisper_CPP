#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fv::audio {

namespace detail {
struct FrameData;
struct DecoderImpl;
struct ResamplerImpl;
} // namespace detail

// A file could not be opened or decoded. what() is a full sentence naming the file's base name
// and FFmpeg's reason.
class AudioDecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The system refused to let the file be read (EACCES or EPERM: missing permissions, or on
// Windows another program holding it open without sharing), which PyAV raises as
// PermissionError.
class AudioFileDenied : public AudioDecodeError {
public:
    using AudioDecodeError::AudioDecodeError;
};

// One decoded frame, in the decoder's own sample format and channel layout, timestamp cleared
// (local_asr/audio.py sets frame.pts = None before resampling).
// Affinity: one thread at a time.
class DecodedFrame {
public:
    DecodedFrame();
    ~DecodedFrame();
    DecodedFrame(DecodedFrame&&) noexcept;
    DecodedFrame& operator=(DecodedFrame&&) noexcept;

    int rate() const;
    int channels() const;
    std::size_t frames() const;
    bool empty() const;

private:
    friend struct detail::DecoderImpl;
    friend struct detail::ResamplerImpl;
    std::unique_ptr<detail::FrameData> data_;
};

// The first audio stream of a WAV, MP3, M4A, OGG, FLAC or WMA file, through FFmpeg as PyAV's
// av.open(path).decode(audio=0) reads it. A mono 16-bit PCM WAV is opened with probesize=4096
// (local_asr/audio.py _MONO_PCM_OPTIONS); the frames are the same, the stop is 17 ms faster.
// Affinity: one thread at a time; blocks on file I/O.
class AudioFileDecoder {
public:
    // Throws AudioDecodeError when the file cannot be opened or has no audio stream.
    explicit AudioFileDecoder(const std::filesystem::path& path);
    ~AudioFileDecoder();
    AudioFileDecoder(AudioFileDecoder&&) noexcept;
    AudioFileDecoder& operator=(AudioFileDecoder&&) noexcept;

    int rate() const;     // the stream's sample rate, Hz
    int channels() const; // the stream's channel count
    // The stream's duration in seconds from its header, else the container's; negative when
    // neither says.
    double duration_s() const;

    // The next frame; false at the end. Throws AudioDecodeError on corrupt data.
    bool next(DecodedFrame& frame);
    // The next frame converted to interleaved int16 at its own rate and layout, appended to
    // `out`; false at the end.
    bool read_s16(std::vector<std::int16_t>& out);

private:
    std::unique_ptr<detail::DecoderImpl> impl_;
};

} // namespace fv::audio
