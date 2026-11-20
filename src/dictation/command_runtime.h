#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "base/clock.h"
#include "base/executor.h"
#include "dictation/command_selection.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/job.h"
#include "dictation/rewrite_jobs.h"
#include "focus/capture_service.h"
#include "focus/clipboard_port.h"
#include "focus/synthetic_keys.h"
#include "llm/credentials.h"
#include "llm/transcript_cleanup.h"
#include "text/transforms.h"

namespace fv {
class SettingsStore;
}

namespace fv::dictation {

class TranscriptionRuntime;

// Command Mode and transforms (services/runtime/command.py CommandRuntime): rewrite the
// selection, or write at the cursor, by voice; and rewrite the selection with a saved transform
// from its shortcut. Command Mode records a spoken instruction like a dictation and its transcript
// comes here (complete_recording) instead of to cleanup; a transform records nothing. Both read
// the selection through command_selection.h, exactly once per recording, and run on the shared AI
// cleanup client inside the transcription job slot, so a Cancel ends them and their result is
// pasted and saved like a dictation's. Selections, instructions and results are never logged.
//
// Affinity: UI actor, except key_pressed and key_released (the hotkey dispatcher, in hook order;
// they run the recording's start, stop and cancel on `ui` and wait for them), complete_recording
// and rewrite_text (the transcription runtime's jobs strand, slot held). Selection reads and the
// transform's provider check, which reads keys, run on `jobs`, which must not be the strand that
// runs complete_recording: that waits up to SELECTION_WAIT for a read it started there.
// Everything given to the constructor outlives the runtime; destroy it after `ui` and `jobs`
// stopped running its tasks.
//
// Shutdown order: cleanup() first, on the UI thread, then the HotkeyRuntime's cleanup and
// set_command_keys(nullptr), then the destructor. A press on the dispatcher waits for `ui` to
// start or stop the recording and gives up only once this runtime is closed, so a UI thread that
// stops or joins the dispatcher before cleanup() can wait on it forever; and resetting the keys
// does not end a press already running here.
class CommandRuntime final : public CommandKeys, public CommandCompletion {
public:
    CommandRuntime(TranscriptionRuntime& runtime, SettingsStore& settings,
                   focus::ClipboardPort& clipboard, focus::SyntheticKeys& keys,
                   focus::CaptureService& capture, std::shared_ptr<llm::TranscriptCleanup> cleanup,
                   Executor& ui, Executor& jobs, Clock& clock, llm::KeySources credentials);
    ~CommandRuntime() override;
    CommandRuntime(const CommandRuntime&) = delete;
    CommandRuntime& operator=(const CommandRuntime&) = delete;

    // The command_mode shortcut went down; `at` is the hook's monotonic time (seconds). Without a
    // provider "Set up AI cleanup to use Command Mode"; during a dictation "Finish the current
    // recording before using Command Mode"; else a Command Mode recording starts. In toggle mode
    // the next press stops it.
    void key_pressed(double at) override;
    // The shortcut came up. Push-and-hold: a release under RECORD_MIN_HOLD_MS cancels the
    // recording, a longer one stops it and starts reading the selection.
    void key_released(double at) override;
    // Rewrites the current selection with the saved transform `transform_id` (its shortcut, so
    // source "hotkey"): "That transform no longer exists", "Set up AI cleanup to use
    // transforms", else the slot is claimed ("Rewriting · <name>...") once the provider check
    // came back from `jobs`, and the selection is read there.
    void run_transform(std::string transform_id);
    // A Command Mode recording's transcript as the text to paste, the selection as its raw text.
    // Throws CommandRefused when there is nothing to do (no instruction, the selection unread or
    // never read in time, nothing selected with writing turned off, no provider, the model
    // refused) and std::runtime_error with the user-facing message when the model fails.
    RewriteResult complete_recording(const std::string& raw,
                                     const std::shared_ptr<const DictationJob>& job) override;
    // Rewrites `text` with a saved instruction, as an action shaped by a transform does with its
    // own dictated words. Throws as complete_recording does.
    RewriteResult rewrite_text(const std::string& text, const std::string& instruction) override;
    // Shortcuts and transforms do nothing from here on, and a press waiting for `ui` returns.
    // Any thread.
    void cleanup();

    // How long complete_recording waits for a selection still being read (SELECTION_WAIT);
    // tests shorten it.
    void set_selection_wait(std::chrono::milliseconds wait) { selection_wait_ms_ = wait.count(); }

private:
    // A Command Mode recording this runtime started (_Recording).
    struct Recording {
        std::shared_ptr<SelectionSource> selection;
        std::shared_ptr<const DictationJob> job;
        double press_at = 0.0;
        bool push_hold = false;
    };
    // Where a recording's selection is read from, and whether that read started (_focus_for,
    // _reading), by the job's id.
    struct SelectionRead {
        std::shared_ptr<SelectionSource> source;
        std::shared_ptr<focus::FocusRequest> focus;
        bool reading = false;
    };

    bool running(const Recording& recording) const;
    void start(double at);
    void refuse(const std::string& message);
    void stop(const Recording& recording);
    void read_selection_once(const std::string& job_id);
    std::shared_ptr<focus::FocusRequest> request_focus();
    void begin_transform(const text::Transform& transform, std::optional<bool> ready,
                         const Json& settings);
    void transform_selected(const std::shared_ptr<const DictationJob>& job,
                            const std::shared_ptr<SelectionSource>& source,
                            const std::string& instruction, SelectionValue value);
    RewriteResult rewrite(const std::string& selection, const std::string& instruction,
                          const std::shared_ptr<const DictationJob>& job);
    // Runs fn on `ui` and waits for its answer (inline when already there); fallback once the
    // runtime is closed while waiting or `ui` drops the task.
    bool call_on_ui(const std::function<bool()>& fn, bool fallback);
    void post_ui(std::function<void()> task);
    void post_jobs(std::function<void()> task);

    TranscriptionRuntime& runtime_;
    SettingsStore& settings_;
    focus::ClipboardPort& clipboard_;
    focus::SyntheticKeys& keys_;
    focus::CaptureService& capture_;
    std::shared_ptr<llm::TranscriptCleanup> cleanup_;
    Executor& ui_;
    Executor& jobs_;
    Clock& clock_;
    llm::KeySources credentials_;
    std::atomic<std::chrono::milliseconds::rep> selection_wait_ms_{SELECTION_WAIT.count()};

    std::atomic<bool> closed_{false};
    mutable std::mutex mutex_;
    // From its start until the shortcut stops or cancels it; only the dispatcher replaces it.
    std::optional<Recording> recording_;
    std::map<std::string, SelectionRead> reads_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace fv::dictation
