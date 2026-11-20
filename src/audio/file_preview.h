#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// What an audio file is before it is transcribed (services/audio_processor.py preview_file):
// size, duration, rate and channels read from the container header, never from a full decode.
namespace fv::audio {

// config.MAX_FILE_SIZE_MB: the largest file one API upload may be, in MiB.
inline constexpr int kMaxFileSizeMb = 23;

// UnreadableAudioError: a file that can't be read as audio. what() is a sentence for the user;
// FFmpeg's own wording goes to the log instead.
class UnreadableAudio : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// FileNotFoundError: "Audio file not found: {path}".
class AudioFileNotFound : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// NOT_AUDIO_MESSAGE.
inline constexpr std::string_view NOT_AUDIO_MESSAGE = "This doesn't look like an audio file.";

// AudioFilePreview. Affinity: value type.
struct AudioFilePreview {
    std::string file_path;     // UTF-8, as given
    std::string file_name;     // its base name, UTF-8
    double file_size_mb = 0.0; // MiB
    double duration_seconds = 0.0;
    int sample_rate = 0;
    int channels = 0;
    // Whether the engine this preview was read for will split the file.
    bool needs_splitting = false;
    int estimated_chunks = 1;
    // Seconds per estimated chunk; empty for a preview built by hand.
    std::vector<double> chunk_durations;

    // Whether the file is bigger than one API upload may be (strictly over kMaxFileSizeMb).
    bool over_upload_limit() const;
    // format_audio_duration: "3.7s", "2m 30s", "1h 3m".
    std::string duration_formatted() const;
    // "X.Y MB" from 1 MiB up, else "N KB".
    std::string file_size_formatted() const;
};

// The split points (sample indices where each chunk after the first starts its unshared part)
// for `total_samples` of audio at `sample_rate`, as AudioProcessor._generate_time_based_splits
// returns them. The splitting engine provides it (speech::UploadSplitter::time_based_splits);
// fv_audio does not depend on fv_speech.
using ChunkEstimator =
    std::function<std::vector<std::int64_t>(std::int64_t total_samples, int sample_rate)>;

// check_file_size: (whether the file is over kMaxFileSizeMb, its size in MiB). Throws
// AudioFileNotFound for a path that does not exist.
struct FileSize {
    bool needs_splitting = false;
    double size_mb = 0.0;
};
FileSize check_file_size(const std::filesystem::path& path);

// AudioProcessor.preview_file. `engine_splits`: whether the engine that will transcribe the
// file splits one over the upload limit. With no estimator a file that needs splitting is still
// reported as one chunk of its whole duration. Throws AudioFileNotFound for a missing path and
// UnreadableAudio with "This file is empty.", "{product} isn't allowed to read this file.", "This
// file has no audio track.", "This file has no audio in it." or NOT_AUDIO_MESSAGE. Affinity: any
// thread except the GUI thread; blocks on file I/O, and decodes the whole file when its header
// gives no duration.
AudioFilePreview preview_file(const std::filesystem::path& path, bool engine_splits,
                              const ChunkEstimator* estimator = nullptr);

// _probe_audio_header's result.
struct AudioHeader {
    double duration_s = 0.0;
    int sample_rate = 0;
    int channels = 0;
};

// _probe_audio_header: duration, rate and channels from the first audio stream's header, else
// the container's duration; a file whose header gives no duration (or rate) is decoded and its
// samples counted. Throws UnreadableAudio for a refused header read, no audio track or no audio
// in it, and AudioFileDenied (file_decoder.h) for a refused decode, which preview_file reports
// as the refused read; any other failure is another exception, which preview_file reports as
// NOT_AUDIO_MESSAGE.
// Affinity: as preview_file.
AudioHeader probe_audio_header(const std::filesystem::path& path);

} // namespace fv::audio
