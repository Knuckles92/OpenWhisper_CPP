#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/cancel.h"

namespace fv::speech {

// The upload limits of services/audio_processor.py (config.MAX_FILE_SIZE_MB and the splitting
// constants). Tests shrink them as OpenWhisper's tests monkeypatch config.
struct SplitLimits {
    double max_file_size_mb = 23;    // MiB one upload may be, WAV header included
    double overlap_s = 2.0;          // shared by neighbouring chunks, at most a quarter window
    double min_chunk_s = 30;         // a cut is searched at least this far into a window
    double silence_s = 0.5;          // the quiet stretch a cut lands in the middle of
    double silence_threshold = 0.01; // peak of the smoothed level that counts as quiet
};

// Centered boxcar mean, np.convolve(x, ones(w)/w, "same") in O(n): differences of float64
// cumulative sums over blocks of 2^22 samples (bounding the working set however long the
// recording is), each block carrying the halo its windows reach into. Agrees with np.convolve
// to 3e-8, nine orders of magnitude under the silence threshold. A window of 1 or less, or no
// samples, returns the input.
std::vector<float> moving_average(std::span<const float> samples, std::size_t window);

// (samples one upload window holds, overlap in samples) at `rate`: (MiB limit - 44 header
// bytes) / 2. Throws std::invalid_argument("Upload size limit is too small for a WAV file")
// under 4 samples.
std::pair<std::size_t, std::size_t> chunk_limits(int rate, const SplitLimits& limits = {});

// The quietest later stretch of `silence_samples` in smooth[start, end) whose peak is under the
// threshold, searched backwards from the end in 0.1 s steps; its middle, or nullopt.
std::optional<std::size_t> find_best_silence(std::span<const float> smooth, std::size_t start,
                                             std::size_t end, std::size_t silence_samples, int rate,
                                             const SplitLimits& limits = {},
                                             const CancelToken& cancel = {});

// Where to cut one window of at most a full upload: a silence at least min_chunk_s past the
// `prefix` already sent, else the window's last safe cut (its length less the overlap).
std::size_t chunk_boundary(std::span<const std::int16_t> window, int rate, std::size_t prefix,
                           std::size_t overlap, const SplitLimits& limits = {},
                           const CancelToken& cancel = {});

// Split points of a whole recording, with the same bounded windows split_audio_file uses.
std::vector<std::size_t> find_split_points(std::span<const std::int16_t> audio, int rate,
                                           const SplitLimits& limits = {});
// Split points every full window less the overlap, as the Upload tab's estimate.
std::vector<std::size_t> time_based_splits(std::size_t total_samples, int rate,
                                           const SplitLimits& limits = {});

// Non-empty chunk transcripts joined with one space, runs of spaces collapsed, stripped.
std::string combine_transcriptions(std::span<const std::string> transcriptions);

// Splits a file over the upload limit into WAV chunks in a temporary folder, decoding once and
// holding at most one upload window plus one frame (audio_processor.AudioProcessor). Each
// operation's folder is its own: cleanup_temp_files removes this splitter's folders only.
// Affinity: one thread at a time; blocks on file I/O, never on the GUI thread.
class UploadSplitter {
public:
    using Progress = std::function<void(const std::string& message)>;

    // Native-rate mono int16 blocks of a file, one per decoded frame; destroying it closes the
    // decoder.
    class BlockSource {
    public:
        virtual ~BlockSource() = default;
        // The next block (replacing `samples`), the stream's rate and channel count; false at the
        // end. Throws when the file cannot be decoded.
        virtual bool next(std::vector<std::int16_t>& samples, int& rate, int& channels) = 0;
    };

    explicit UploadSplitter(SplitLimits limits = {});
    virtual ~UploadSplitter();
    UploadSplitter(const UploadSplitter&) = delete;
    UploadSplitter& operator=(const UploadSplitter&) = delete;

    const SplitLimits& limits() const { return limits_; }

    // (needs splitting, size in MiB). Throws EngineError("Audio file not found: <path>").
    virtual std::pair<bool, double> check_file_size(const std::filesystem::path& audio) const;

    // The chunk files in order. Reports "Decoding and splitting audio..." and "Created audio
    // chunk <n>..."; a cancel throws EngineError("Transcription canceled"). On any failure this
    // operation's folder is removed before the error propagates.
    virtual std::vector<std::filesystem::path> split_audio_file(const std::filesystem::path& audio,
                                                                const Progress& progress,
                                                                const CancelToken& cancel);

    // Removes every temporary file and folder made so far; one it cannot remove is logged
    // ("Failed to cleanup temp file ...") and forgotten.
    virtual void cleanup_temp_files();
    const std::vector<std::filesystem::path>& temp_files() const { return temp_files_; }

protected:
    // Seams, as OpenWhisper's tests monkeypatch the processor's methods.
    virtual std::unique_ptr<BlockSource> open_blocks(const std::filesystem::path& audio,
                                                     const CancelToken& cancel);
    // A mono 16-bit PCM WAV, as Python's wave module writes it (44-byte header).
    virtual void save_chunk(std::span<const std::int16_t> samples, int rate,
                            const std::filesystem::path& file);
    // tempfile.mkdtemp(prefix="audio_chunks_") in the system's temporary folder.
    virtual std::filesystem::path make_temp_dir();
    // Removes one file, or a folder and its contents; throws std::filesystem::filesystem_error.
    virtual void remove_path(const std::filesystem::path& path);
    virtual std::size_t boundary(std::span<const std::int16_t> window, int rate, std::size_t prefix,
                                 std::size_t overlap, const CancelToken& cancel);

private:
    void split_blocks(BlockSource& blocks,
                      const std::function<void(std::span<const std::int16_t>, int)>& save,
                      const CancelToken& cancel);
    void discard_temp_dir(const std::filesystem::path& dir);

    SplitLimits limits_;
    std::vector<std::filesystem::path> temp_files_;
};

} // namespace fv::speech
