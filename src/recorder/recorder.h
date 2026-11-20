#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "audio/backend.h"
#include "audio/devices.h"
#include "audio/pcm_tape.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/paths.h"
#include "base/signal.h"
#include "recorder/journal.h"
#include "recorder/post_roll.h"

namespace fv {

class SettingsStore;

// The dictation format (config.py SAMPLE_RATE, CHANNELS, AUDIO_FORMAT int16, CHUNK_SIZE).
inline constexpr int RECORDER_SAMPLE_RATE = 44100;
inline constexpr int RECORDER_CHANNELS = 1;
inline constexpr int RECORDER_SAMPLE_WIDTH = 2; // bytes: int16
inline constexpr int RECORDER_CHUNK = 1024;     // frames per device block
// The level meter's exponential smoothing (config.WAVEFORM_LEVEL_SMOOTHING): level =
// 0.3 * previous + 0.7 * block RMS.
inline constexpr double WAVEFORM_LEVEL_SMOOTHING = 0.3;
// The saved recording's name in the data root (config.RECORDED_AUDIO_FILE).
inline constexpr std::string_view RECORDED_AUDIO_FILE = "recorded_audio.wav";

inline constexpr std::string_view NO_MICROPHONE_LEFT =
    "The microphone stopped and no other microphone could be opened; recording stopped.";

// Every wait and limit of the recorder, in one place so tests can shorten them as OpenWhisper's
// tests patch the module constants.
struct RecorderTimings {
    // The post-roll cap (POST_ROLL_MS). Waiting all of it was 1.22-1.25 s of a ~1.4 s
    // stop-to-paste in OpenWhisper's September 2026 latency audit.
    std::chrono::milliseconds post_roll{1200};
    // Added to the cap for wait_for_stop_completion's default (POST_ROLL_FINALIZE_GRACE_MS).
    std::chrono::milliseconds finalize_grace{800};
    // A stream may take a while to deliver its first block: a Bluetooth headset switching to
    // its hands-free profile takes seconds.
    std::chrono::milliseconds first_block_grace{3000};
    // Once blocks flow (one every 23 ms), this long without one means the microphone is gone.
    // Waiting longer loses more speech.
    std::chrono::milliseconds stall_after_blocks{1000};
    // Silence written where one microphone's audio ends and the next one's starts, so a speech
    // model hears a pause instead of two words run together.
    std::chrono::milliseconds switch_gap{250};
    // Closing a stream on an unplugged device can hang in the driver.
    std::chrono::milliseconds lost_stream_close{1000};
    // An input overflow is a short gap and the recording carries on; this many within the
    // window means the computer cannot keep up, and the recording stops.
    int overflow_limit = 3;
    std::chrono::milliseconds overflow_window{5000};
    // Trailing silence on a saved WAV: some speech models drop the last word without it.
    std::chrono::milliseconds end_padding{500};
    // How often the watcher looks for a lost microphone or a storage error.
    std::chrono::milliseconds watch_interval{100};
    // How long cleanup() waits for the recording to end, and for each journal release.
    std::chrono::milliseconds cleanup_join{500};
};

struct RecorderOptions {
    // Empty: paths.user_file(RECORDED_AUDIO_FILE). Secondary recorders pass a private path so
    // they never overwrite the dictation's.
    std::filesystem::path output_file;
    // A raw PortAudio index tried before anything else.
    std::optional<int> device_id;
    // Ranked microphones: each recording starts on the first that opens and moves down the
    // list when that one is lost, ending at the system default.
    std::vector<audio::DeviceKey> device_priority;
    PostRollConfig post_roll;
    RecorderTimings timings;
    JournalOptions journal;
    // Wall time for the WAV's origination tags and the journal; system_clock() when null.
    const Clock* clock = nullptr;
};

// What the recorder has captured and how its post-roll stands, read in one go.
struct RecorderSnapshot {
    std::int64_t recorded_frames = 0;
    std::uint64_t recorded_bytes = 0;
    PostRollGate post_roll_gate{RECORDER_SAMPLE_RATE};
    // Why the post-roll ended early ("quiet", "paused", "cancel", "capture error", "cleanup",
    // "microphone lost"); empty while it has not.
    std::string post_roll_end_reason;
    bool stop_requested = false;
    // The post-roll cap of the pending stop: its deadline minus the stop press.
    std::optional<std::chrono::steady_clock::duration> post_roll_cap;
    int device_switches = 0;
};

namespace recorder_detail {
struct RecorderImpl;
}

// Dictation capture (services/recorder.py AudioRecorder; also walkthroughs, the microphone
// check and the cleanup-rules dialog) with device fallback, adaptive post-roll, pause, cancel,
// a crash-safe journal and the atomic WAV save.
//
// Threads: the device callback only copies each block into a preallocated ring; the recorder's
// "fv-audio-io" thread drains it into the journal, the tape, the post-roll gate and the level
// meter; a "fv-recorder-watch" thread per recording waits for the stop and the post-roll, moves
// to the next microphone when one is lost, then closes the stream and finishes the journal.
// Signals are emitted on those threads.
// Affinity: thread-safe; methods are called from the UI executor or workers as in Python.
// start_recording, stop_recording and the save block on devices and disk, briefly.
// `backend` must outlive the recorder and any lost microphone's close still running (it lives
// as long as the process in the app).
class Recorder {
public:
    Recorder(audio::Backend& backend, const Paths& paths, RecorderOptions options = {});
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // The dictation recorder for the saved microphone order. Never lists devices on the calling
    // thread: a legacy microphone index still to migrate is resolved on `io`, and the recorder
    // uses the system default until it is ("Couldn't migrate the saved microphone" is logged when
    // the migration fails). `settings` and `backend` must outlive that task.
    static std::unique_ptr<Recorder> from_settings(SettingsStore& settings, audio::Backend& backend,
                                                   const Paths& paths, Executor& io);
    // Every audio input PortAudio lists, loopback devices excepted; [] (logged) when the audio
    // system cannot be read.
    static std::vector<audio::InputDevice> get_input_devices(audio::Backend& backend);
    // A device or open error as the user reads it: PortAudio-ish errors are "No audio device
    // available", anything else its own text, or "Could not open the audio stream".
    static std::string format_start_error(std::string_view message);

    // Opens the input stream before the session counts as recording; false (and
    // last_start_error) when no microphone opens or one is already recording.
    bool start_recording();
    std::string last_start_error() const;
    // Requests the stop: capture continues until the tail is quiet or the cap. A repeat request
    // during the post-roll keeps the first cap; false when not recording.
    bool stop_recording();
    // Keeps nothing the microphone hears until resume(); false when not recording, stopping or
    // cancelled. Paused time is not in the recording at all.
    bool pause();
    void resume();
    bool paused() const;
    // Waits for capture and storage, each stage within its own deadline; the default covers the
    // cap, the finalize grace and the journal's finish timeout. False on timeout ("audio kept
    // for recovery" is logged).
    bool wait_for_stop_completion(std::optional<std::chrono::milliseconds> timeout = std::nullopt);
    // Writes the captured PCM, 500 ms of silence and the origination tags to `file` (default
    // output_file()) atomically; the previous file stays until the new one is complete. Refuses
    // a capture that lost audio unless `allow_incomplete`, and any journal failure.
    bool save_recording(std::optional<std::filesystem::path> file = std::nullopt,
                        bool allow_incomplete = false);
    double get_recording_duration() const; // seconds
    bool has_recording_data() const;
    // The PCM captured from byte `offset` on, as the WAV will hold it, committed by the journal
    // writer; nullopt when the capture no longer reaches `offset` (cleared or restarted).
    std::optional<std::string> read_recorded_bytes(std::int64_t offset) const;
    // Throws the capture away: ends the post-roll now, keeps nothing after it and discards the
    // journal without waiting for storage.
    void cancel_recording();
    // True from cancel_recording until the next recording starts.
    bool capture_canceled() const;
    bool is_recording() const;
    // Releases capture resources; an unacknowledged session stays on disk for recovery unless
    // `discard`. Without `wait` the journal is released on its own thread.
    void clear_recording_data(bool discard = false, bool wait = true);
    // Removes the journal, once history and audio are durably saved.
    void acknowledge_recording();
    void cleanup();

    const std::filesystem::path& output_file() const;
    // The current recording's samples as they are captured (live preview and incremental
    // dictation); null when there is no capture.
    std::shared_ptr<const audio::PcmTape> tape() const;
    // The current recording's journal; null when there is no capture.
    std::shared_ptr<RecordingJournal> journal() const;
    std::uint64_t dropped_frames() const;
    std::uint64_t input_overflows() const;
    // The one failure this recording reported; empty when none.
    std::string last_capture_error() const;
    // Ends the recording with `message` as its one failure, as a device failure does
    // (AudioRecorder._fail_capture): for a caller whose stop timed out. Any thread.
    void fail_capture(const std::string& message);
    // The microphone this recording is on; nullopt is PortAudio's default input.
    std::optional<audio::InputDevice> active_device() const;
    std::vector<audio::DeviceKey> device_priority() const;
    void set_device_priority(std::vector<audio::DeviceKey> priority);
    RecorderSnapshot snapshot() const;
    // Waits until every block handed over so far is counted, metered, on the tape and written
    // to the journal; false on timeout.
    bool sync_capture(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

    int rate() const;
    int channels() const;
    int chunk() const;

private:
    std::shared_ptr<recorder_detail::RecorderImpl> impl_;

public:
    // Smoothed 0..1 level of each kept block.
    Signal<float>& level;
    // The one visible failure of a recording.
    Signal<std::string>& capture_error;
    // (old, new) short microphone names after a mid-recording switch; "" is the system default.
    Signal<std::string, std::string>& device_switched;
};

} // namespace fv
