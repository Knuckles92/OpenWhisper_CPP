#pragma once

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "automation/control.h"
#include "base/clock.h"
#include "base/json.h"
#include "base/signal.h"

// The desktop app's AppControl for the live API and MCP (services/automation/app_control.py).
// The listener calls in from its own threads. Reading the app's state happens there; anything
// that starts, stops or shows something runs on the UI thread: the caller posts the call to the
// UI executor and waits up to CALL_TIMEOUT for it to be taken (when it is not, it never runs),
// then up to RUNNING_GRACE more once it is running, so the answer is what happened rather than a
// guess.
namespace fv {
class Executor;
class SettingsStore;
} // namespace fv

namespace fv::agentapi {

// How long a caller waits for the UI thread to take its call.
inline constexpr std::chrono::milliseconds CALL_TIMEOUT{2000};
// How much longer it waits once the call is running there.
inline constexpr std::chrono::milliseconds RUNNING_GRACE{2000};

// What UiAppControl needs from the running app; implemented by the controller (M5.22).
// Affinity: state() on any thread, everything else on the UI thread.
class ControlHost {
public:
    virtual ~ControlHost() = default;
    // Read without waiting for the UI thread: recording (the recorder records), busy (a job
    // holds the slot until its history is saved), meeting, and the mode, action id and job id of
    // the current job (as routing chose it) while recording or busy, with the app's version.
    virtual automation::LiveState state() = 0;
    // What the record shortcut starts, with `source` as its source (TranscriptionRuntime's
    // start_recording), or a recording for the action (start_action_recording). The new job's
    // id ("" when it has none) once it started; nullopt when the app refused, after saying why on
    // status_updates().
    virtual std::optional<std::string> start_recording(std::optional<std::string> action_id,
                                                       std::string source) = 0;
    // Stops the recording, whose words are transcribed and delivered as usual; false when
    // nothing is recording.
    virtual bool stop_recording() = 0;
    // Cancels the recording or the job still working (never the user's Cancel, which also
    // stops their actions); false when nothing can be cancelled or the cancel was refused.
    virtual bool cancel() = 0;
    // TranscriptionRuntime::run_action_text: the job's id. May throw dictation::RunRefused or
    // dictation::RunPermissionDenied, which the control reports as ControlError and
    // PermissionDenied.
    virtual std::string run_action_text(std::string action_id, std::string text,
                                        std::string source) = 0;
    // Shows a one-line message: the status line and a notice near the pointer.
    virtual void notify(std::string line) = 0;
    // The status texts the app shows (the controller's status_update), captured during a start.
    virtual Signal<std::string>& status_updates() = 0;
};

// QtAppControl: the registered AppControl of the running app. Calls after release() are
// "unavailable", so a registration left behind never reaches a controller that is gone.
// Affinity: thread-safe; its methods block for up to CALL_TIMEOUT + RUNNING_GRACE, so they are
// never called from the GUI thread except by the UI executor's own tasks, which run at once.
class UiAppControl final : public automation::AppControl {
public:
    // `ui` runs the calls (the GUI thread's executor); `settings` gives the saved actions;
    // `clock` times the waits. All must outlive the control.
    UiAppControl(ControlHost& host, Executor& ui, SettingsStore& settings,
                 const Clock& clock = system_clock());

    automation::LiveState state() override;
    // The enabled actions' public views.
    Json list_actions() override;
    std::string run_action(std::string_view action_id, std::string_view text,
                           std::string_view source) override;
    // Refused while recording, busy or in a meeting, and for an action that is not enabled or
    // that would hand a program's words to a coding agent without api_agent_prompts.
    std::string start_recording(std::optional<std::string> action_id,
                                std::string_view source) override;
    void stop_recording() override;
    void cancel() override;
    // Whitespace runs become one space; "title: text" when there is a title.
    void notify(std::string_view text, std::string_view title = {}) override;

    // Forgets the host (the controller is going away); on the UI thread, never inside a call.
    void release();

private:
    template <class Fn> auto call_on_ui(Fn fn);
    ControlHost& require();

    std::shared_mutex mutex_; // guards host_ against release()
    ControlHost* host_;
    Executor& ui_;
    SettingsStore& settings_;
    const Clock& clock_;
};

} // namespace fv::agentapi
