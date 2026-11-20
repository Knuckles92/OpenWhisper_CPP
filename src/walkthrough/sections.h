#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

// Cutting a walkthrough's recording into one clip per section (services/walkthrough/
// sections.py). Each screenshot closes a section at the moment it was kept; the recorder's frame
// count matches the saved WAV sample for sample, so a moment is an exact place in the file. The
// cut then moves to the quietest tenth of a second nearby, so a word the user was finishing as
// they kept the screenshot stays whole. Stretches held for a note are taken out of the sections
// and cut as clips of their own, exactly where held.
namespace fv::walkthrough {

// How far a cut may move from the moment its screenshot was kept, in seconds.
inline constexpr double REACH_S = 0.75;
// The stretch of quiet a cut looks for, in seconds; windows step by half of it.
inline constexpr double FRAME_S = 0.1;
// A cut moves only to a window at most this loud next to the mark's own, so steady speech with
// no real pause is cut where the screenshot was kept.
inline constexpr double QUIET_RATIO = 0.5;
// Shorter clips hold no words worth sending to an engine, in seconds.
inline constexpr double MIN_CLIP_S = 0.25;

// 16-bit PCM as recorded: interleaved samples, channels kept (the cut search sums energy over
// them, which averaging to mono would change).
struct Pcm16 {
    std::vector<std::int16_t> samples;
    int rate = 0;
    int channels = 1;

    std::int64_t frames() const {
        return channels > 0 ? static_cast<std::int64_t>(samples.size()) / channels : 0;
    }
};

// Python's wave.Error: the file is not a PCM WAV this reader understands ("file does not start
// with RIFF id", "unknown format: 3", ...). Like OSError and ValueError, it sends
// transcribe_sections to its whole-file fallback.
class WaveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A WAV that ends inside a header, or a chunk that runs past the RIFF chunk: Python's wave
// raises EOFError or RuntimeError there, which no caller catches.
class WaveTruncated : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Reads a WAV as Python's wave module does (PCM or extensible PCM, chunks bounded by the RIFF
// chunk, a short data chunk read as far as it goes). Throws std::invalid_argument("expected
// 16-bit audio, got N-bit") for another sample width, WaveError, WaveTruncated, or
// std::filesystem::filesystem_error when it cannot be read.
Pcm16 read_pcm16_wav(const std::filesystem::path& path);

// The frame each section ends at, one per mark (seconds, in order), never decreasing nor
// passing the next mark.
std::vector<std::int64_t> cut_frames(const Pcm16& audio, std::span<const double> marks_s);

// Where to cut in [low, high): the middle of its quietest FRAME_S window (nearest target on
// ties), or target clamped to the range when no window is clearly quieter (_quietest).
std::int64_t quietest(const Pcm16& audio, std::int64_t low, std::int64_t high, std::int64_t target);

// [start, end) less every gap (in order, not overlapping), as spans in order (_without).
std::vector<std::pair<std::int64_t, std::int64_t>>
without(std::int64_t start, std::int64_t end,
        std::span<const std::pair<std::int64_t, std::int64_t>> gaps);

struct Clip {
    // The section's audio as a WAV, or nullopt when it is too short to decode.
    std::optional<std::filesystem::path> path;
    double duration_s = 0;

    friend bool operator==(const Clip&, const Clip&) = default;
};

// One clip per section, out_dir/section-<i>.wav from 1: before the first cut, between cuts, and
// after the last, each less the stretches in leave_out_s ((start, end) seconds in time order; an
// end of +inf runs to the last frame). Clips are 16-bit at the recording's rate and channels.
// Throws as read_pcm16_wav, and std::system_error when a clip cannot be written.
std::vector<Clip> split_recording(const std::filesystem::path& wav, std::span<const double> cuts_s,
                                  const std::filesystem::path& out_dir,
                                  std::span<const std::pair<double, double>> leave_out_s = {});

// One clip per stretch, out_dir/note-<i>.wav from 1, cut exactly there. Throws as
// split_recording.
std::vector<Clip> clip_stretches(const std::filesystem::path& wav,
                                 std::span<const std::pair<double, double>> stretches_s,
                                 const std::filesystem::path& out_dir);

} // namespace fv::walkthrough
