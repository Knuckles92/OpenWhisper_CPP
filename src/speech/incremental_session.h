#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "audio/pcm_tape.h"
#include "audio/resampler.h"
#include "base/cancel.h"
#include "dsp/window_splitter.h"
#include "engine/recognizer.h"
#include "engine/transcript.h"
#include "speech/service.h"

// A long dictation's completed windows decoded while it is still recorded
// (services/incremental_dictation.py, without the native stream's stream_final).
//
// FileWindows cuts a saved WAV into windows of at most 30 s, each split chosen from the audio
// before it alone, after one Resampler16k. A window complete while the user still speaks is
// therefore cut identically from the saved file, provided the 16 kHz stream matches bit for bit.
// A session follows the recorder's tape through its own Resampler16k and WindowSplitter and
// decodes each window as it completes, so the stop waits only for the last partial window.
//
// The saved file stays the source of truth: at stop the session checks that the audio it
// followed is exactly the start of the saved WAV, takes the rest from the file, and checks each
// window against the file's own. Any doubt discards the early text and the caller decodes the
// whole file, so the transcript is always the one that path would produce.
namespace fv::speech {

// INCREMENTAL_DICTATION_POLL_SEC.
inline constexpr std::chrono::milliseconds kIncrementalPollInterval{1000};

// Whether a backend decodes long dictations early (INCREMENTAL_DICTATION_BACKENDS without the
// dropped engines).
bool decodes_incrementally(std::string_view backend_id);

// _context_key: what a final pass must share with the early windows to reuse them. A missing
// context is ("", []).
struct ContextKey {
    std::string language;
    std::vector<std::string> phrases;
    bool operator==(const ContextKey&) const = default;
};
ContextKey context_key(std::string_view language, std::span<const std::string> phrases);

// One recording's early decode (DictationSession).
//
// Its thread ("incremental-dictation") polls the tape every poll interval until finish(),
// discard() or destruction; once 30 s are captured it feeds the new samples through the
// resampler and splitter and decodes each completed window on Lane::Incremental with the
// session's language and phrases. Any doubt invalidates the session: the dictation slot's
// generation moved (a reload, switch, release or a settings language change, which reaches the
// engine only through a new select), a decode failed, or discard() was called (a cancel, a
// microphone switch, a meeting: the dictation runtime decides).
//
// Affinity: thread-safe. poll() runs on the session's thread, or on another (calls are serialized,
// so a session whose thread waits a long poll interval can be followed by hand); finish() blocks
// and runs on a worker thread; discard() never waits.
// The service must outlive the session.
class IncrementalSession {
public:
    // A session for the recording on `tape`, or null where it cannot help: a backend that does
    // not decode incrementally, a tape that is not mono, or an engine that is not Ready.
    // `options` and `phrases` are the dictation's recognition context.
    static std::unique_ptr<IncrementalSession>
    create(SpeechService& service, std::shared_ptr<const audio::PcmTape> tape,
           const RecognitionOptions& options, std::string backend_id,
           std::vector<std::string> phrases = {},
           std::chrono::milliseconds poll = kIncrementalPollInterval);

    // Stops and joins the thread.
    ~IncrementalSession();
    IncrementalSession(const IncrementalSession&) = delete;
    IncrementalSession& operator=(const IncrementalSession&) = delete;

    void start();
    // Drops the early text and stops following; never waits for a window in flight (a queued
    // one is cancelled).
    void discard(std::string reason = "discarded");

    // Takes newly captured audio and decodes any window it completes. Returns whether to keep
    // polling: false once stopped or invalidated.
    bool poll();

    // Whether a final pass with this context decodes like the early windows.
    bool matches(const RecognitionOptions& options,
                 std::span<const std::string> phrases = {}) const;

    // The saved recording's transcript, or nullopt to decode the file instead: nothing was
    // decoded early (a short dictation falls straight through), or the early text cannot be
    // proven to match. Stops polling and waits for a window in flight; `cancel` (the job's)
    // cancels that window and every decode at stop. The windows left at stop are decoded on
    // Lane::Final. Segment and word times are seconds in the file. Blocking: a worker thread.
    std::optional<Transcript> finish(const std::filesystem::path& wav,
                                     const CancelToken& cancel = {});

    // Windows decoded before finish().
    std::size_t early_windows() const;
    // Why the session was abandoned; nullopt while it is valid.
    std::optional<std::string> invalid_reason() const;
    // Seconds the engine spent on this session's windows, early and at stop.
    double decode_seconds() const;
    // Whether its thread is still running (started and not yet returned).
    bool following() const;

private:
    struct Fingerprint {
        double offset_s = 0.0;
        std::size_t samples = 0;
        std::string digest; // SHA-256 of the float32 samples' bytes
        bool operator==(const Fingerprint&) const = default;
    };

    IncrementalSession(SpeechService& service, std::shared_ptr<const audio::PcmTape> tape,
                       RecognitionOptions options, std::vector<std::string> phrases,
                       std::chrono::milliseconds poll);

    static Fingerprint fingerprint(const AudioClip& clip);
    void run();
    // Sets stop_ under mutex_ and wakes the thread, so a wake-up between its check and its wait
    // is never lost (it would sleep out a whole poll interval).
    void request_stop();
    void add_decode_seconds(double seconds);
    void invalidate(const std::string& reason);
    std::optional<std::string> stale() const;
    void push(std::span<const std::int16_t> samples);
    void decode_ready();
    DecodeResult decode(const AudioClip& window, Lane lane, const CancelToken& cancel);
    std::vector<AudioClip> verified_final_windows(const std::filesystem::path& wav);

    SpeechService& service_;
    std::shared_ptr<const audio::PcmTape> tape_;
    const RecognitionOptions options_;
    const std::vector<std::string> phrases_;
    const std::chrono::milliseconds poll_;
    const std::uint64_t generation_;

    mutable std::mutex mutex_; // guards invalid_, early_ and decode_s_; stop_ is set under it
    std::condition_variable wake_;
    std::optional<std::string> invalid_;
    std::size_t early_ = 0;
    double decode_s_ = 0.0;
    std::atomic<bool> stop_{false};
    std::atomic<bool> following_{false};
    bool finished_ = false; // finish() ran: the resampler is flushed
    CancelSource decodes_;  // cancels this session's queued or running window
    std::thread thread_;

    // Under poll_mutex_: the polling state, touched by poll() and, once it has joined the thread,
    // finish().
    std::mutex poll_mutex_;
    std::optional<audio::Resampler16k> resampler_;
    dsp::WindowSplitter splitter_;
    std::int64_t consumed_ = 0; // tape frames already fed to the resampler
    std::uint32_t crc_ = 0;     // CRC-32 of their bytes
    std::vector<AudioClip> ready_;
    std::vector<Transcript> transcripts_;
    std::vector<Fingerprint> fingerprints_;
};

// The transcription runtime's slot for the dictation being recorded (IncrementalDictation).
// Affinity: thread-safe; transcribe() blocks and runs on a worker thread.
class IncrementalDictation {
public:
    explicit IncrementalDictation(SpeechService& service);
    ~IncrementalDictation();
    IncrementalDictation(const IncrementalDictation&) = delete;
    IncrementalDictation& operator=(const IncrementalDictation&) = delete;

    // Follows the recording that just started on `tape`, saved later to `recording_file`, where
    // its engine allows; a session already held is discarded ("a new recording started").
    void start(std::shared_ptr<const audio::PcmTape> tape, const RecognitionOptions& options,
               std::string backend_id, std::filesystem::path recording_file,
               std::vector<std::string> phrases = {},
               std::chrono::milliseconds poll = kIncrementalPollInterval);
    // Drops the session of a canceled recording without waiting on it.
    void discard();
    // decode_file(path) on Lane::Final, reusing this dictation's early windows when `path` is
    // the recording the session followed and the context is the one they decoded with; an
    // upload or a retranscribe takes the plain path and drops any session.
    DecodeResult transcribe(const std::filesystem::path& path, const RecognitionOptions& options,
                            const CancelToken& cancel = {},
                            std::span<const std::string> phrases = {});

    // The session held now (tests and the runtime's logging); null when none.
    std::shared_ptr<IncrementalSession> session() const;

private:
    // Keeps a dropped session until its thread has returned, so that dropping it never waits
    // for a window in flight; destroys those whose thread is done.
    void retire(std::shared_ptr<IncrementalSession> session);

    SpeechService& service_;
    mutable std::mutex mutex_;
    std::shared_ptr<IncrementalSession> session_;
    std::filesystem::path recording_file_;
    std::vector<std::shared_ptr<IncrementalSession>> retired_;
};

} // namespace fv::speech
