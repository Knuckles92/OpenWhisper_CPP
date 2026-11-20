#pragma once

#include <string>

#include "base/signal.h"

namespace fv::dictation {

// What the hotkey runtime asks of the recording side: the controller's recording entry points
// (application_controller.py start_recording .. minimize_to_tray) and the recorder's state.
// Implemented by the transcription runtime (M1.14) and the controller (M1.16).
//
// Affinity: the state queries are thread-safe (the hotkey dispatcher reads them while it decides
// a press or release). The actions are called on the UI executor only: HotkeyRuntime posts them
// there and, where Python's call returns an answer, waits for it off the UI thread.
// recording_state_changed is emitted on the UI thread.
class RecordingControl {
public:
    virtual ~RecordingControl() = default;

    // recorder.is_recording: from the accepted start until a stop's post-roll ends.
    virtual bool is_recording() const = 0;
    // The recorder still holds a capture after is_recording turned false (its stream is still
    // closing): a new start now would be refused. The hotkey runtime treats it as recording.
    virtual bool recorder_busy() const = 0;
    // TranscriptionRuntime.canceling_recording: true only while a Cancel announces that the
    // recording stopped (recording_state_changed(false) is being emitted for a cancel).
    virtual bool canceling_recording() const = 0;
    // recorder.capture_canceled: a Cancel already threw this capture away and its stream is only
    // closing; a stop now must not claim a job.
    virtual bool capture_canceled() const = 0;
    // The capture's input stream is open. A start may be accepted before it is.
    virtual bool stream_open() const = 0;

    // start_recording(source=...): false when the start was refused (its status already says
    // why). `source` is "hotkey" or "ui" (catalog.SOURCES).
    virtual bool start(std::string source) = 0;
    // stop_recording: stop and transcribe.
    virtual void stop() = 0;
    // toggle_recording: false when a start was refused.
    virtual bool toggle() = 0;
    // The user's Cancel; true when a recording or job is being stopped.
    virtual bool cancel() = 0;
    // minimize_to_tray: toggle the main window between the tray and the foreground.
    virtual void minimize_to_tray() = 0;
    // The "Paste original" shortcut, once its keys are up: paste the last transcript before AI
    // cleanup (ui_controller.paste_last_original).
    virtual void paste_last_original() = 0;

    // A recording started (true) or ended for any reason: stop, cancel, tray, error (false).
    virtual Signal<bool>& recording_state_changed() = 0;
};

} // namespace fv::dictation
