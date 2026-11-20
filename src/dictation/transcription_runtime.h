#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "appcore/overlay_state.h"
#include "base/cancel.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "dictation/automation_port.h"
#include "dictation/batch.h"
#include "dictation/cleaner_port.h"
#include "dictation/cleanup_info.h"
#include "dictation/cleanup_stage.h"
#include "dictation/delivery.h"
#include "dictation/job.h"
#include "dictation/job_slot.h"
#include "dictation/pipeline.h"
#include "dictation/recorder_port.h"
#include "dictation/recording_control.h"
#include "dictation/rewrite_jobs.h"
#include "dictation/scratchpad_port.h"
#include "dictation/uploads.h"
#include "dictation/walkthrough_delivery.h"
#include "dictation/walkthrough_job.h"
#include "history/manager.h"
#include "remote/link.h"
#include "text/cleanup_profiles.h"
#include "walkthrough/session.h"

namespace fv {
class ActivityState;
class EventSink;
class SettingsStore;
namespace appcore {
class PreviewThrottle;
}
namespace dictation_stats {
class StatsRepository;
}
namespace llm {
class TranscriptCleanup;
}
namespace speech {
class IncrementalDictation;
class SpeechService;
} // namespace speech
} // namespace fv

namespace fv::dictation {

class StreamingRuntime;

// What the stats strip shows after a transcription (ui_controller.set_transcription_stats).
// Affinity: value type.
struct TranscriptionStats {
    double transcription_time = 0.0; // seconds
    double audio_duration = 0.0;     // seconds; 0 when unknown
    std::int64_t file_size = 0;      // bytes; 0 when unknown
    std::optional<double> cleanup_time;
    // Where a remote engine's final pass spent its time ("on <host> · 31 ms network").
    std::optional<remote::RemoteTiming> remote;

    bool operator==(const TranscriptionStats&) const = default;
};

// How a history save ended (controller.history_persisted's dict).
// Affinity: value type.
struct HistoryOutcome {
    // Shown on the status line when not empty.
    std::string error;
    // History changed: the sidebar reloads.
    bool refresh = false;
    // Shown near the pointer when not empty.
    std::string notice;

    bool operator==(const HistoryOutcome&) const = default;
};

// The one-shot metadata of the job holding the slot (controller._pending_*), set when its audio
// is known and cleared when the job ends.
// Affinity: value type.
struct PendingAudio {
    // A recording's own WAV; none for retranscriptions (and uploads, M2), which are read from a
    // file the user keeps.
    std::optional<std::filesystem::path> audio_path;
    std::optional<double> audio_duration;  // seconds
    std::optional<std::int64_t> file_size; // bytes
    std::optional<std::string> source_name;
    // How long the final pass took (controller._transcription_elapsed), seconds.
    std::optional<double> transcription_time;
    // The final pass's requests to a remote engine (controller._remote_timing).
    std::optional<remote::RemoteTiming> remote_timing;

    bool operator==(const PendingAudio&) const = default;
};

// What a recording is for (start_recording's arguments).
// Affinity: value type.
struct StartOptions {
    JobMode mode = JobMode::Dictation;
    // For Command Mode, the selected text to rewrite.
    SelectionFuture selection;
    // Who asked (catalog.SOURCES): "hotkey", "ui", or a program's "api" or "mcp".
    std::string source = "hotkey";
    // "hotkey" to route the words by voice; a dictation gets "auto" itself when routing listens
    // to every dictation.
    std::string route;
    // The cleanup profile to format this recording with; "" for none. The profile and the
    // settings it reads are taken as the recording starts. An Action recording takes its
    // action's profile instead.
    std::string profile_id;
    // The saved action an Action recording's words go to; it must exist and be enabled as the
    // recording starts.
    std::string action_id;
};

// What the runtime asks of the composition (M1.16). An empty function does nothing (a readiness
// message of nothing lets capture start; stop_playback defaults to audio::stop_playback).
struct RuntimeHooks {
    // controller.transcription_readiness_message: why capture must not start yet. UI thread.
    std::function<std::optional<std::string>()> readiness_message;
    // EngineReloadMachine::request. Any thread (a worker finds the engine not ready).
    std::function<void()> request_reload;
    // The model download consent flow. Any thread.
    std::function<void()> request_model_consent;
    // Stops History playback before the microphone opens. UI thread.
    std::function<void()> stop_playback;
    // RecordingControl::minimize_to_tray and paste_last_original, which the controller does.
    std::function<void()> minimize_to_tray;
    std::function<void()> paste_last_original;
    // The dictation slot's backend's Backend::large_file_size_mb: a file's size in MiB when that
    // backend splits it for upload (capabilities().split_over_mb), else nullopt; throws as the
    // backend does. Empty: no backend splits. Jobs strand.
    std::function<std::optional<double>(const std::filesystem::path&)> large_file_size_mb;
    // Starts the dictation slot's backend's prepare_for_transcription on a thread of its own
    // when the backend prepares_for_transcription(), and returns at once. UI thread, as a
    // recording starts.
    std::function<void()> prepare_for_transcription;
};

// transcription.py REMOTE_RECOVERY_DELAYS_S and REMOTE_RECOVERY_WAIT_S: a recording that stops
// while its paired host is away waits this long, in all, for up to three reconnects, the pause
// before each the delay at its index.
inline constexpr std::array<std::chrono::milliseconds, 3> REMOTE_RECOVERY_DELAYS{
    std::chrono::milliseconds(0), std::chrono::milliseconds(1000), std::chrono::milliseconds(2000)};
inline constexpr std::chrono::milliseconds REMOTE_RECOVERY_WAIT{12000};

// Everything the runtime reaches; every pointer is required and must outlive the runtime.
struct TranscriptionServices {
    SettingsStore* settings = nullptr;
    RecorderPort* recorder = nullptr;
    speech::SpeechService* speech = nullptr;
    HistoryManager* history = nullptr;
    // The one StatsRepository of the database (HistoryManager::add_entry records no stats).
    dictation_stats::StatsRepository* stats = nullptr;
    DeliveryPort delivery;
    // The refusal gates read it; the runtime sets `recording` with every
    // recording_state_changed and `transcribing` while the slot is held.
    ActivityState* activity = nullptr;
    // The UI executor (ctx.ui()), the jobs strand (decodes, the stop's post-roll and save) and
    // the persistence strand (history saves, one at a time).
    Executor* ui = nullptr;
    Executor* jobs = nullptr;
    Executor* persistence = nullptr;
    // null_event_sink() when null.
    EventSink* events = nullptr;
    // focus::get_service() when null.
    focus::CaptureService* capture = nullptr;
    // system_clock() when null; its steady time measures stop-to-result and cancel.
    const Clock* clock = nullptr;
    RuntimeHooks hooks;
    // <data root>/captures (walkthrough::captures_root), where startup recovery joins
    // interrupted walkthroughs to their recovered audio and prunes expired ones; empty skips
    // both.
    std::filesystem::path captures_root;
};

// The recording flow and the single transcription job slot (services/runtime/transcription.py
// TranscriptionRuntime), for dictation: start, stop, cancel, the final decode through the
// speech service, delivery (paste or copy), History and Stats, failed-recording retention,
// History retry, startup recovery, uploads and batches of uploads, Scratchpad delivery, the live
// preview's start, stop, fallback and cancel (through a StreamingRuntime), and the early decode
// of a long dictation (its speech::IncrementalDictation slot), the dictionary, snippets and AI
// cleanup of every transcript (cleanup_stage.h), cleanup profiles, learning from corrections
// after a paste, and the rewrites of Command Mode and transforms (a Command Mode recording's
// transcript goes to the CommandCompletion; a transform claims the slot with begin_rewrite_job).
// Through an AutomationPort, a dictation or action job's words pass the text filters before and
// after cleanup, a dictation may be routed by voice to a saved action, an Action job's words are
// shaped as its action asks and delivered to it, and run_action_text runs an action on words a
// program sent. A walkthrough records like a dictation; once it stops, its recording is cut at
// its screenshots, each section decoded and cleaned on its own, and its documents written; its
// AI write-up then runs off the slot, on the walkthrough-library strand, so dictation keeps
// working while the agent writes (walkthrough_job.cpp). The remote engine (M9) arrives later.
//
// The early decode follows every recording but a walkthrough; a recording that is not
// transcribed (a cancel, a failed stop or save, a capture error, a job that ends) drops it, since
// a tape never ends on its own.
//
// One job at a time: a stop claims the slot on the UI thread before the post-roll, so nothing
// else starts while the WAV is written; the decode runs on `jobs`; the result is delivered on
// the UI thread (paste first) and saved on `persistence`; the History save's end frees the slot.
// An upload or a batch of uploads claims the same slot; its result stays in the window (the
// transcript view and History), never pasted or copied.
//
// Affinity: UI actor. Exceptions: the state queries (has_active_job, cancelable,
// canceling_recording, is_transcribing, current_job, pending, upload_job_active,
// recording_walkthrough, writing_up, the RecordingControl queries), begin_shutdown,
// publish_preview and report_backend_progress are thread-safe; finish_recording_job,
// transcribe_audio_file, transcribe_batch and recover_recordings run on a worker. Signals are
// emitted on the UI thread or a worker: connect them on the executor that should run them.
// Destroy it only after the jobs, persistence and walkthrough strands stopped running its tasks.
class TranscriptionRuntime final : public RecordingControl {
public:
    explicit TranscriptionRuntime(TranscriptionServices services);
    ~TranscriptionRuntime() override;
    TranscriptionRuntime(const TranscriptionRuntime&) = delete;
    TranscriptionRuntime& operator=(const TranscriptionRuntime&) = delete;

    // RecordingControl, for HotkeyRuntime.
    bool is_recording() const override;
    bool recorder_busy() const override;
    bool canceling_recording() const override;
    bool capture_canceled() const override;
    bool stream_open() const override;
    bool start(std::string source) override;
    void stop() override;
    bool toggle() override;
    bool cancel() override;
    void minimize_to_tray() override;
    void paste_last_original() override;
    // A recording started (true) or ended for any reason (false).
    Signal<bool>& recording_state_changed() override { return recording_state_changed_; }

    // A recording; false when it was refused or could not start (the status says why).
    bool start_recording(StartOptions options = {});
    // Stops capture and transcribes: "Processing..." at once, the post-roll and save on `jobs`.
    void stop_recording();
    // Starts when not recording, else stops; false when a start was refused.
    bool toggle_recording();
    // History's Retry transcription and Re-transcribe: a failed recording's retry completes its
    // own entry instead of adding one.
    void retranscribe_audio(std::filesystem::path audio_path);
    // Startup worker: makes interrupted dictation audio available in Recordings, joins
    // interrupted walkthroughs to their recovered audio and prunes expired ones (captures_root).
    void recover_recordings();

    // Records a walkthrough whose screenshots `session` keeps; false when refused (the status
    // says why). The session belongs to the job from here: a Cancel deletes its screenshots,
    // any other end keeps them. `source` is who asked ("hotkey", "ui", "tray").
    bool start_walkthrough(std::shared_ptr<walkthrough::WalkthroughSession> session,
                           std::string source = "hotkey");
    // The walkthrough taking screenshots now, or null; null once its recording stopped, when it
    // is transcribed. Any thread.
    std::shared_ptr<walkthrough::WalkthroughSession> recording_walkthrough() const;
    // Finishes the interrupted walkthrough in `folder` (a draft with its recovered audio.wav)
    // through the usual job: "Recovering walkthrough...", sections, documents and delivery.
    // Refused during a meeting or a recording, while another job holds the slot, and while a
    // write-up or a library change runs. Reads the draft and the WAV header on the calling
    // thread, as Python does on its Qt thread.
    void recover_walkthrough(std::filesystem::path folder);
    // Whether the AI write-up of a finished walkthrough runs. Any thread.
    bool writing_up() const;
    // Stops that write-up; its walkthrough is then copied as spoken. False when none runs.
    bool cancel_write_up();
    // The write-up came back, failed or was canceled: copies what there is (nothing once
    // quitting). The runtime calls it on the UI executor itself after walkthrough_written_up.
    void on_walkthrough_written_up(walkthrough::WalkthroughResult result);
    // Where finished walkthroughs go; null (the default) copies nothing and shows no card. Must
    // outlive the runtime or be reset first. UI thread.
    void set_walkthrough_delivery(WalkthroughDeliveryPort* delivery);
    // The walkthrough-library strand, where AI write-ups run one at a time off the job slot;
    // null (the default) queues them on the jobs strand. Must outlive the runtime or be reset
    // while no write-up runs. UI thread.
    void set_walkthrough_strand(Executor* strand);
    // The write-up itself; empty (the default) is write_up_walkthrough. Set it while no write-up
    // runs. UI thread.
    void set_walkthrough_write_up(WalkthroughWriteUp write_up);
    // RuntimeHooks::readiness_message's answer: why capture must not start yet, or nullopt.
    // UI thread.
    std::optional<std::string> readiness_message() const;
    // The Upload File tab's single file: "Processing uploaded file..." at once, the decode on
    // `jobs`, the result shown and saved but never pasted or copied. `duration_s` is the file
    // preview's (seconds), saved with the entry. False when refused (a meeting, a missing file,
    // a recording or another job) or when it could not start; the status says why.
    bool upload_audio_file(std::filesystem::path audio_path,
                           std::optional<double> duration_s = std::nullopt);
    // Several uploaded files as one job holding the slot throughout, decoded one after another
    // on `jobs` (transcribe_batch); one file takes upload_audio_file. False when refused or when
    // it could not start; the status says why.
    bool upload_audio_files(BatchUploadRequest request);
    // A backend's step inside a decode (its on_progress): the status, and the Transcribing
    // overlay from the step that starts the transcription proper. Any thread.
    void report_backend_progress(std::string message, bool transcribing = false);
    // An upload or a batch holds the slot (the runtime's half of ui_controller
    // _upload_job_active: its stages belong on the Upload card, not the overlay).
    bool upload_job_active() const;
    // The batch's worker: emits batch_progress and batch_item_finished per file, then
    // batch_completed and hands the result to on_batch_complete on the UI thread; a failure or a
    // canceled combined batch goes to on_transcription_error. Jobs strand.
    void transcribe_batch(BatchUploadRequest request);
    // A finished batch: the transcript view, the stats, one History entry per file with words
    // (or one for a combined batch), the status, then the slot is freed by the History save.
    // UI thread.
    void on_batch_complete(BatchResult result);

    // The Scratchpad a dictation goes to while it is focused; null (the default) leaves
    // DeliveryPort::insert_into_scratchpad. Must outlive the runtime or be reset first. UI thread.
    void set_scratchpad(ScratchpadPort* scratchpad);
    // The AI cleanup pass; null (the default) is the cleaner set_cleanup made, else
    // null_transcript_cleaner(). Must outlive the runtime or be reset while no job runs. UI thread.
    void set_cleaner(TranscriptCleaner* cleaner);
    // AI cleanup through `cleanup`, used while set_cleaner gave no cleaner; null drops it. Set
    // it while no job runs. UI thread.
    void set_cleanup(std::shared_ptr<llm::TranscriptCleanup> cleanup);
    // The dictionary and snippets of every transcript and the learning after a paste; null (the
    // default) is dictionary_preparer(), which does not learn. Must outlive the runtime or be
    // reset while no job runs. UI thread.
    void set_text_preparer(const TextPreparer* preparer);
    // The id of the cleanup profile the recording (or the job it became) formats with, ""
    // without one. Any thread.
    std::string recording_profile() const;
    // The preparer set_text_preparer gave, else dictionary_preparer(). Any thread.
    const TextPreparer& text_preparer() const;
    // Where Command Mode and transform recordings' transcripts go (CommandRuntime); null (the
    // default) fails them with "Command Mode isn't available". Must outlive the runtime or be
    // reset while no job runs. Any thread.
    void set_command_runtime(CommandCompletion* command);

    // Claims the slot for a rewrite of selected text (a command or transform job carrying the
    // selection): "Rewriting..." with the Rewriting overlay, and History names the entry's source
    // `source_name`. Refused while recording or while another job runs: a job the user started
    // says it is busy near the pointer, a program's says nothing (its own answer reports it).
    // The caller then either submits the rewrite or abandons it. UI thread; never waits.
    bool begin_rewrite_job(std::shared_ptr<const DictationJob> job, std::string source_name);
    // Runs `work` on `jobs`; its result takes a dictation's path (pasted, even with auto-paste
    // off, into the app it started in, and saved), a CommandRefused shows its message as it is,
    // any other failure "Error: <message>", and either frees the slot. UI thread.
    void submit_rewrite(RewriteWork work);
    // Frees a slot claimed by begin_rewrite_job that was never submitted, showing `status` on the
    // status line and near the pointer (none after a Cancel, which said "Canceled" already).
    // Does nothing once the slot is no longer that rewrite's. UI thread.
    void abandon_rewrite_job(std::string status);
    // The model download consent flow (RuntimeHooks::request_model_consent), started when a
    // job finds the speech model missing. Any thread; the hook runs on the thread that found it.
    void set_model_consent_hook(std::function<void()> hook);
    // The Remote engine's link (RemoteRuntime.retry_for_transcription): reconnect now, and
    // whether a reconnect is running; both called from the jobs strand. Unset: a Remote engine
    // that is not connected fails a recording at once.
    void set_remote_link(std::function<void()> retry_now, std::function<bool()> restoring);
    // Times a Remote engine's final pass for the stats line (transcription.py _timed_decode's
    // timing_mark and timing_since): called on the jobs strand just before the decode, it
    // returns what then reads the requests made since (nullopt when none can be read).
    using RemoteTimingStart = std::function<std::function<std::optional<remote::RemoteTiming>()>()>;
    void set_remote_timing(RemoteTimingStart start);
    // Pastes `text` at the caret even with auto-paste off, saving nothing. Refused while
    // recording or while another job delivers; `job` is the job `text` answers, which may still
    // hold the slot while it is saved (a routed copy of the job, with its id, counts as it).
    bool paste_text_now(std::string text, const DictationJob* job = nullptr);

    // Text filters, Route by voice and saved actions; null_automation_port() (the default) has
    // none. Must outlive the runtime or be replaced while no job runs. Any thread.
    void set_automation(AutomationPort& automation);
    void set_automation(std::shared_ptr<AutomationPort> automation);
    // Runs the saved action `action_id` on `text` without recording: the words take a
    // dictation's path from there (filters, the action's shaping, delivery to the action, and
    // History) on `jobs`. `source` is who asked ("api", "mcp", or the user's own). Returns the
    // job's id, which its events carry. Throws RunRefused: "refused" for no words, "busy" during
    // a meeting, while actions take no new runs or while another job or a recording holds the
    // slot, "not_found" for no enabled action with that id; and RunPermissionDenied when a
    // program asks for an action that does not take a program's words. A program's refusal is
    // never shown to the user: the caller's answer is the report. UI thread.
    std::string run_action_text(std::string_view action_id, std::string_view text,
                                std::string_view source);
    // The engine selection changed to `backend_id` ("parakeet", "api", "remote"): saved, and
    // the engine reloads. Refused during a meeting.
    void on_model_changed(std::string backend_id);
    // The recorder's capture_error: previews stop and the captured audio is kept, never pasted.
    // A notification from a canceled or replaced capture is ignored.
    void on_capture_error(std::string message);
    // Quit: the current job's work is cancelled, whatever its state.
    void begin_shutdown();

    // A result is queued, transcribing, cleaning or being saved.
    bool has_active_job() const;
    // A Cancel has something to stop: a recording, or a job still working on its result.
    bool cancelable() const;
    // The engine is decoding this runtime's job.
    bool is_transcribing() const;
    // The slot's job once a stop or retry claimed it, else the recording's.
    std::shared_ptr<const DictationJob> current_job() const;
    // transcription.preview for `job` while it records; dropped once its recording stopped or
    // was canceled. Never in the replay buffer. Any thread.
    void publish_preview(const DictationJob* job, std::string_view text, bool final);
    // publish_preview for the job with this id, as the preview throttle hands it on. Any thread.
    void publish_preview(std::string_view job_id, std::string_view text, bool final);

    // The live preview; null (the default) records without one. Its streaming_text_update is
    // published as transcription.preview, at most four a second (PreviewThrottle on its clock).
    // Must outlive the runtime or be reset first, while nothing records. UI thread.
    void set_streaming(StreamingRuntime* streaming);
    // The early decode of the dictation being recorded (controller._incremental).
    speech::IncrementalDictation& incremental_dictation() { return *incremental_; }
    // A meeting started or the microphone order changed (the recorder is rebuilt): the recording's
    // early decode is dropped and its file decoded whole. Any thread; never waits.
    void discard_early_decode();

    // The worker's results (controller.transcription_completed and transcription_failed), and
    // the end of a History save. UI thread.
    void on_transcription_complete(std::string transcript,
                                   std::optional<std::string> raw_text = std::nullopt,
                                   std::optional<CleanupInfo> cleanup_info = std::nullopt);
    void on_transcription_error(std::string error_message);
    void on_history_persisted(HistoryOutcome outcome);

    // _claim_job: reserves the slot for `job` (null for a file) unless a job holds it, quitting,
    // a backup or a paired update refuses, or (require_idle_recorder) the recorder records.
    bool claim_job(std::shared_ptr<const DictationJob> job = nullptr,
                   bool require_idle_recorder = false);
    // _finish_job: frees the slot and forgets everything the job held. Any thread.
    void finish_job();
    // The slot, for the runtimes that share it (Command Mode, M4).
    JobSlot& job_slot() { return slot_; }

    // The stop's work off the UI thread: post-roll wait, WAV save and the final pass. Owns the
    // slot the stop claimed and releases it on every exit. Jobs strand.
    void finish_recording_job();
    // The final pass of `audio_path` for the slot's job, then its result to the UI thread.
    // Jobs strand.
    void transcribe_audio_file(std::filesystem::path audio_path);

    // The current job's metadata. set_pending is for jobs that start from a file (M2 uploads).
    PendingAudio pending() const;
    void set_pending(PendingAudio pending);
    // The stopped live preview's text, for the empty-transcript fallback; "" without one.
    std::string pending_streaming_text() const;

    // The status line (controller.status_update and ui_controller.set_status).
    Signal<std::string> status_update;
    // transcription_retry_progress: "Retrying connection (n/3)..." for the overlay.
    Signal<std::string> transcription_retry_progress;
    // show_transcription_retry_failure: the failed recording's notice ("Reconnect failed. Audio
    // saved in History.") once a remote engine's reconnects ran out; sent instead of
    // notice_requested, because the overlay shows it even while the main window is in front.
    Signal<std::string> transcription_retry_failed;
    Signal<appcore::OverlayState> overlay_state_update;
    // What the overlay says the recording is for ("", "Walkthrough", "Agent · ...").
    Signal<std::string> recording_destination_changed;
    // The transcript view (ui_controller.set_transcript): the text shown (EMPTY_ASR_MESSAGE when
    // nothing was heard), the words before cleanup when cleanup changed them, and the cleanup.
    Signal<std::string, std::optional<std::string>, std::optional<CleanupInfo>>
        transcription_completed;
    // ui_controller.show_transcription_error: the failure's status, for the error dialog.
    Signal<std::string> transcription_failed;
    Signal<TranscriptionStats> transcription_stats;
    // ui_controller.clear_transcription_stats, as a recording starts.
    Signal<> clear_stats;
    // main_window.clear_partial_transcription: Quick Record's preview text, as a recording
    // starts.
    Signal<> clear_partial_transcription;
    // After the History save, before the slot is freed.
    Signal<HistoryOutcome> history_persisted;
    // A near-pointer notice (text, done) for an outcome the status line can't show.
    Signal<std::string, bool> notice_requested;
    // ui_controller.refresh_history. A subscriber that throws is logged; the slot is freed.
    Signal<> refresh_history;
    // ui_controller.set_device_info after an engine change: (readout, engine usable).
    Signal<std::string, std::optional<bool>> device_info_changed;
    // A batch's files, from its worker: (1-based position, total, source name) as each starts.
    Signal<int, int, std::string> batch_progress;
    // (position, succeeded, the file's own stripped transcript) as each file ends; the text is
    // empty on failure and in a combined batch, whose files have no finished text of their own.
    Signal<int, bool, std::string> batch_item_finished;
    // The finished batch, from its worker, before on_batch_complete runs on the UI thread.
    Signal<BatchResult> batch_completed;
    // A file the backend splits for upload starts: its size in MiB. From the worker.
    Signal<double> large_file_detected;
    // A walkthrough's AI write-up ended: the result written up, or as spoken with its
    // write_up_error. From the walkthrough-library strand; the runtime then runs
    // on_walkthrough_written_up on the UI executor itself, so listeners only observe it.
    Signal<walkthrough::WalkthroughResult> walkthrough_written_up;

private:
    struct QueuedEntry;

    Json settings_snapshot() const;
    bool can_start_recording();
    std::string report_busy(std::string_view action);
    void report_start_failure();
    void show_listening(JobMode mode, std::optional<std::string_view> action_name,
                        std::optional<std::string_view> profile_name);
    std::shared_ptr<AutomationPort> automation() const;
    void forget_recording();
    // Starts the early decode of the recording `job` is for. UI thread.
    void start_early_decode(const DictationJob& job, const Json& settings);
    // The stop's preview text (stop_streaming_session), after the post-roll. Jobs strand.
    void stop_preview();
    // _complete_preview_fallback: the stopped preview's whole text for an empty dictation.
    void complete_preview_fallback(const std::filesystem::path& audio_path, const std::string& raw);
    void announce_recording(bool recording);
    void notify(const std::string& text, bool done = false);
    void cancel_recording();
    void cancel_transcription();

    // The walkthrough job (walkthrough_job.cpp, and its decode beside decode_and_complete).
    std::shared_ptr<walkthrough::WalkthroughSession> walkthrough_session() const;
    // Lets go of the walkthrough: its files stay (end) unless the user canceled it (discard).
    void end_walkthrough(bool keep);
    // Stops a canceled walkthrough between sections, or says which one is next. Jobs strand.
    void report_section(int index, int total);
    // _deliver_walkthrough and _copy_walkthrough. UI thread.
    void deliver_walkthrough(const std::string& transcript);
    void start_write_up(const walkthrough::WalkthroughSession& session, const Json& settings);
    void copy_walkthrough(const walkthrough::WalkthroughResult& result);
    WalkthroughDeliveryPort* walkthrough_delivery() const;
    // " — screenshots kept in <folder>" for a walkthrough's error status, or "".
    std::string walkthrough_kept_note() const;

    std::optional<std::int64_t> saved_recording_size(bool stop_completed);
    std::string recording_source_name(const DictationJob* job) const;
    void run_transcription_job(const std::filesystem::path& audio_path);
    void require_backend_ready();
    // _retry_remote_for_recording: joins reconnects of the paired host for a bounded time;
    // throws EngineNotReadyError when it stays away.
    void retry_remote_for_recording();
    void submit_transcription_job(const std::filesystem::path& audio_path);
    void abandon_canceled_job(std::string_view stage);
    void raise_if_canceled() const;
    std::shared_ptr<const DictationJob>
    with_final_recognition(std::shared_ptr<const DictationJob> job,
                           std::optional<RecognitionContext>& recognition);
    struct Completion;
    Completion decode_and_complete(const std::filesystem::path& audio_path,
                                   const std::optional<RecognitionContext>& recognition,
                                   const std::shared_ptr<const DictationJob>& job);
    // _complete_walkthrough: decodes, cleans and writes the walkthrough section by section;
    // the words with a marker where each screenshot sits. Jobs strand.
    Completion complete_walkthrough(const std::filesystem::path& audio_path,
                                    const std::optional<RecognitionContext>& recognition);
    // _clean_sections: each section's words after the dictionary, snippets and AI cleanup.
    struct CleanedSections;
    CleanedSections clean_sections(const std::vector<std::string>& texts);
    Completion maybe_cleanup_transcript(const std::string& raw,
                                        const std::optional<std::string>& batch_context = {},
                                        std::optional<double> timeout_s = std::nullopt);
    // _maybe_cleanup_transcript with a profile chosen after the recording (nullopt for none), on
    // fresh settings; allow_ai false skips AI cleanup (an action shaped "raw").
    Completion cleanup_as(const std::string& raw, std::optional<text::CleanupProfile> profile,
                          bool allow_ai = true);
    Completion clean(const std::string& raw, CleanupStageInput input);

    // The automation stages of a dictation or Action job's words (_complete_live and its
    // helpers). `job` is updated in place when routing or a program's filter changes it. Jobs
    // strand.
    Completion complete_live(const std::string& raw, std::shared_ptr<const DictationJob> job);
    using FilterCall = std::function<FilterResult(std::string, const DictationJob&)>;
    // The text after `run`, or nullopt when a filter took over delivery.
    std::optional<std::string> filtered(const FilterCall& run, const std::string& text,
                                        std::shared_ptr<const DictationJob>& job);
    std::optional<std::string> run_filters(std::string_view stage, const std::string& text,
                                           std::shared_ptr<const DictationJob>& job,
                                           const Json& settings);
    std::optional<std::string> filter_and_route(const std::string& raw,
                                                std::shared_ptr<const DictationJob>& job,
                                                const Json& settings);
    std::string route_words(const std::string& text, std::shared_ptr<const DictationJob>& job,
                            const Json& settings);
    Completion shape_action(const std::string& text, const DictationJob& job, const Json& settings);
    // History's source name for an Action job: "Action · {name}", or "Action".
    std::string action_source_name(const DictationJob& job) const;
    // The cleaner for one pass, kept alive for it.
    std::shared_ptr<TranscriptCleaner> cleaner() const;
    const TextPreparer& preparer() const;
    void request_model_consent();
    // Shows the stage a file starts in: the large-file notice for a file the backend splits,
    // else Transcribing. Jobs strand.
    void announce_transcription(const std::filesystem::path& audio_path);
    // One file's transcript from the dictation slot's engine: window by window for a local
    // engine, the whole file for one that takes files. Jobs strand.
    std::string decode_path(const std::filesystem::path& audio_path,
                            const std::optional<RecognitionContext>& recognition);
    std::string transcribe_path(const BatchItem& item);
    void report_failure(std::string message);

    void deliver(const std::string& transcript, const std::shared_ptr<const DictationJob>& job,
                 const std::string& notice, const std::optional<std::string>& before_cleanup);
    bool insert_into_scratchpad(const std::string& transcript, const std::string& status_suffix);
    void copy_rewrite(const std::string& transcript, bool app_changed);
    void notify_forced_outcome(const DictationJob& job, bool pasted);
    // _refuse: a command or transform with nothing to paste, reported as a refusal. Worker.
    void refuse(const std::string& message);
    void drop_instruction_audio();
    void refuse_rewrite(const DictationJob& job);
    void run_rewrite(const RewriteWork& work);
    void set_delivery_status(const std::string& status);
    bool apply_clipboard_and_paste(const std::string& transcript,
                                   const std::string& status_suffix = {},
                                   const std::string& html = {}, bool force_paste = false);

    Json entry_stats_counts(const DictationJob* job) const;
    Json history_context(const DictationJob* job, const std::optional<CleanupInfo>& info,
                         bool live) const;
    std::string model_info_for_history() const;
    void queue_history(std::vector<QueuedEntry> entries);
    std::vector<QueuedEntry>
    batch_history_entries(const BatchResult& result,
                          const std::vector<const BatchItemResult*>& saveable) const;
    void publish_batch_completions(std::vector<QueuedEntry>& entries);
    void persist_history(const std::vector<QueuedEntry>& entries);
    void save_failed_recording(const std::filesystem::path& path, std::string status,
                               const PendingAudio& pending, bool remote_retry_exhausted);
    void preserve_failed_audio(const std::filesystem::path& path, std::string status);
    void acknowledge_if_recording(const std::filesystem::path& path);
    void clear_pending_audio_metadata();

    void post_ui(std::function<void()> task);
    void post_jobs(std::function<void()> task);
    double seconds_since(std::chrono::steady_clock::time_point start) const;

    TranscriptionServices services_;
    SettingsStore& settings_;
    RecorderPort& recorder_;
    speech::SpeechService& speech_;
    HistoryManager& history_;
    dictation_stats::StatsRepository& stats_;
    ActivityState& activity_;
    Executor& ui_;
    Executor& jobs_;
    Executor& persistence_;
    EventSink& events_;
    const Clock& clock_;

    JobSlot slot_;
    std::atomic<bool> canceling_recording_{false};
    std::atomic<bool> decoding_{false};
    // The slot holds an upload or a batch.
    std::atomic<bool> upload_job_{false};
    std::atomic<ScratchpadPort*> scratchpad_{nullptr};
    std::atomic<TranscriptCleaner*> cleaner_{nullptr};
    std::atomic<const TextPreparer*> preparer_{nullptr};
    std::atomic<CommandCompletion*> command_{nullptr};
    std::atomic<std::shared_ptr<AutomationPort>> automation_{
        std::shared_ptr<AutomationPort>(&null_automation_port(), [](AutomationPort*) {})};

    // What the job holding the slot carries between the UI thread and its worker.
    mutable std::mutex state_mutex_;
    PendingAudio pending_;
    std::string pending_streaming_text_;
    std::optional<std::chrono::steady_clock::time_point> transcription_started_;
    std::optional<std::chrono::steady_clock::time_point> stop_started_;
    std::optional<std::chrono::steady_clock::time_point> cancel_started_;
    // Set by the worker before the result is posted.
    std::string delivery_html_;
    std::string spoken_text_;
    Json stats_counts_ = Json::object();
    std::optional<std::string> last_cleanup_failure_;
    // The cleanup profile the recording started with, and the settings it read then
    // (_recording_profile, _profile_settings); cleared when the recording is forgotten or its
    // job ends.
    std::optional<text::CleanupProfile> recording_profile_;
    std::optional<Json> profile_settings_;
    // The profile the last cleanup pass formatted with (_formatting_profile), for its notice.
    std::optional<text::CleanupProfile> formatting_profile_;
    // set_cleanup's cleaner.
    std::shared_ptr<TranscriptCleaner> llm_cleaner_;
    bool retranscription_ = false;
    std::optional<std::string> retry_entry_id_;
    // Dictation lands in the app the hotkey was pressed in; a retry from History stays in the
    // window, whose transcript view has its own Copy.
    bool deliver_to_clipboard_ = true;
    // The failure being reported is a refusal (CommandRefused).
    bool refused_ = false;
    // A text filter answered that it handled the words: they are saved, never delivered. Set by
    // the worker.
    bool filter_canceled_ = false;
    // Route by voice's judge could not answer, so the words are pasted and a notice says why.
    // Set by the worker.
    bool route_failed_ = false;
    // Why an action got other words than it asked for (its transform could not run), shown with
    // its outcome. Set by the worker.
    std::string shaping_notice_;
    // begin_rewrite_job claimed the slot and nothing was submitted or abandoned yet.
    bool rewrite_pending_ = false;
    bool remote_retry_exhausted_ = false;
    // set_remote_link's; state_mutex_.
    std::function<void()> retry_remote_;
    std::function<bool()> remote_restoring_;
    RemoteTimingStart remote_timing_;
    // The last status apply_clipboard_and_paste showed. UI thread.
    std::string delivery_status_;
    // RuntimeHooks::request_model_consent, or what set_model_consent_hook gave. Under
    // state_mutex_.
    std::function<void()> model_consent_;
    // The walkthrough job's session, from its start until the job ends (_walkthrough). Under
    // state_mutex_.
    std::shared_ptr<walkthrough::WalkthroughSession> walkthrough_;
    // Stops the AI write-up while one runs (_write_up_cancel); null when none runs. Under
    // state_mutex_.
    std::shared_ptr<CancelSource> write_up_cancel_;
    // set_walkthrough_write_up's function. Under state_mutex_.
    WalkthroughWriteUp write_up_;
    std::atomic<WalkthroughDeliveryPort*> walkthrough_delivery_{nullptr};
    std::atomic<Executor*> walkthrough_strand_{nullptr};

    Signal<bool> recording_state_changed_;
    Subscription capture_link_;
    std::unique_ptr<speech::IncrementalDictation> incremental_;
    std::atomic<StreamingRuntime*> streaming_{nullptr};
    std::unique_ptr<appcore::PreviewThrottle> preview_throttle_; // UI thread
    Subscription preview_link_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace fv::dictation
