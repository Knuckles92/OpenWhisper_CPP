#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/clock.h"

namespace fv::audio {
class WavWriter;
}

// Bounded capture writer and recovery of interrupted Quick Record sessions
// (services/recording_journal.py). Whoever captures only enqueues owned PCM bytes; filesystem
// work and periodic durability checkpoints belong to the writer thread. A session stays
// recoverable until its consumer acknowledges persistence, or the user cancels it.
//
// On disk, per recording: <dir of the output>/.<output stem>-recovery/<32 hex>/ holding
// active.lock (an OS lock, released even after a hard kill), session.json
// ({"version": 1, "rate", "channels", "sample_width", "created": local ISO time}), audio.pcm
// (raw interleaved little-endian PCM) and, once cancelled, discarded.
namespace fv {

inline constexpr std::size_t JOURNAL_QUEUE_BLOCKS = 512;
inline constexpr std::chrono::milliseconds JOURNAL_CHECKPOINT{1000};
inline constexpr std::chrono::milliseconds JOURNAL_FINISH_TIMEOUT{3000};
// How long the writer waits for a block before it looks for a discard.
inline constexpr std::chrono::milliseconds JOURNAL_POLL{50};
inline constexpr std::size_t JOURNAL_COPY_BYTES = 1024 * 1024;

// "Recording storage cannot keep up; recording stopped."
extern const char* const STORAGE_CANNOT_KEEP_UP;

// recovery_directory: <output's resolved parent>/.<output stem>-recovery.
std::filesystem::path recovery_directory(const std::filesystem::path& output_file);

// The file audio.pcm is written through: a seam so tests can stand in for a full, slow or
// short-writing disk, as OpenWhisper's tests replace the journal's file object.
// Affinity: the journal's writer thread (close also from the releasing thread).
class JournalFile {
public:
    virtual ~JournalFile() = default;
    // One write; returns the bytes taken. Throws std::system_error.
    virtual std::size_t write(std::span<const std::byte> data) = 0;
    virtual void sync() = 0;
    virtual void close() = 0;
};

struct JournalOptions {
    std::size_t queue_blocks = JOURNAL_QUEUE_BLOCKS;
    std::chrono::milliseconds checkpoint = JOURNAL_CHECKPOINT;
    // finish()'s and close()'s default wait.
    std::chrono::milliseconds finish_timeout = JOURNAL_FINISH_TIMEOUT;
    std::chrono::milliseconds poll = JOURNAL_POLL;
    // The wall time "created" reads (local time in the clock's zone); system_clock() when null.
    const Clock* clock = nullptr;
    // Wraps the real audio.pcm file (tests); none uses it as is.
    std::function<std::unique_ptr<JournalFile>(std::unique_ptr<JournalFile>)> wrap_file;
};

namespace recorder_detail {
struct JournalState;
}

// Affinity: thread-safe. append never blocks: a full queue drops the block and stops the
// journal. The writer thread ("fv-journal-writer") outlives this object while it still has
// work: a journal destroyed without close() stops accepting blocks, writes what is queued and
// releases its files, keeping the session for recovery unless it is empty.
class RecordingJournal {
public:
    // Creates the session folder (mode 0700), takes its lease and writes session.json. Throws
    // std::system_error or std::filesystem::filesystem_error when storage refuses; the lease is
    // released then. `on_error` runs once on the writer thread when the journal ends with an
    // error (unless it was discarded).
    RecordingJournal(const std::filesystem::path& output_file, int rate, int channels,
                     int sample_width, std::function<void(const std::string&)> on_error,
                     JournalOptions options = {});
    ~RecordingJournal();
    RecordingJournal(const RecordingJournal&) = delete;
    RecordingJournal& operator=(const RecordingJournal&) = delete;

    // Queues a copy of `payload`; false (and nothing queued) once stopped, after an error, or
    // when the queue is full (which sets STORAGE_CANNOT_KEEP_UP and stops the journal).
    bool append(std::span<const std::byte> payload);
    // Stops accepting blocks and waits for the writer to drain and sync; false on timeout.
    bool finish();
    bool finish(std::chrono::milliseconds timeout);
    // The PCM written from byte `offset` on; nullopt when `offset` is past what is written.
    std::optional<std::string> read_from(std::int64_t offset) const;
    // finish(), then the written whole frames into `wav`. Throws std::runtime_error ("Recording
    // storage did not finish; audio kept for recovery") on timeout, std::system_error when the
    // file is short or unreadable.
    void copy_into(audio::WavWriter& wav);
    // Ends the session: with `discard`, a "discarded" marker is synced first, so recovery
    // honours the cancel even if the app dies before the writer cleans up. False when the
    // writer did not finish in time: the lease is kept and the writer releases (and, when
    // discarded, removes) the session when it unblocks. A session left with no audio is
    // removed.
    bool close(bool discard = false);
    // The writer stops and removes the session at its next look, without the marker
    // (clear_recording_data(wait=False) before close runs on another thread).
    void request_discard();
    // Sets the journal's error, as storage that stopped writing does; with `stop`, nothing more
    // is accepted either.
    void report_error(const std::string& message, bool stop = false);
    // Waits until every block queued so far has been written (queue.join()); false on timeout.
    bool wait_until_written(std::chrono::milliseconds timeout) const;

    std::optional<std::string> error() const;
    std::uint64_t written_bytes() const;
    std::uint64_t dropped_frames() const;
    int frame_bytes() const;
    const std::filesystem::path& directory() const;
    const std::filesystem::path& path() const; // audio.pcm

private:
    std::shared_ptr<recorder_detail::JournalState> state_;
};

// Converts abandoned journals beside `output_file` into ordinary WAVs in `recordings_folder`
// (recording_<YYYYmmdd_HHMMSS>-recovered-<session>.wav), skipping sessions a live process still
// holds and removing cancelled or empty ones. Destination names are idempotent, so a retry after
// a crash between the WAV's rename and the session's removal recovers nothing twice; a failure
// leaves the session's PCM for a later retry. Returns the recovered WAVs.
// Affinity: any thread; blocks on disk I/O, never on the GUI thread.
std::vector<std::filesystem::path>
recover_recordings(const std::filesystem::path& output_file,
                   const std::filesystem::path& recordings_folder);

// The recovered WAV's path for a session whose session.json holds `created` (an ISO time);
// creates the folder. Throws py::ValueError for a time fromisoformat refuses.
std::filesystem::path recovered_wav_path(const std::string& created,
                                         const std::filesystem::path& recordings_folder,
                                         const std::filesystem::path& session_directory);

// (rate, channels, sample_width) of a session.json's text, validated as recovery requires
// (rate 8000..384000, 1 or 2 channels, 1 to 4 bytes per sample); throws py::ValueError("Invalid
// recovery audio format"), py::JsonError for text that is not JSON, and std::out_of_range for a
// missing key.
struct SessionFormat {
    int rate = 0;
    int channels = 0;
    int sample_width = 0;
};
SessionFormat session_format(const std::string& session_json);

// session.json's text for a session created at `created` (local time), as json.dump writes it.
std::string session_json(int rate, int channels, int sample_width,
                         std::chrono::local_time<std::chrono::microseconds> created);

} // namespace fv
