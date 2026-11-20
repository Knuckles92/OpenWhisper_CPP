#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "walkthrough/runtime_ports.h"

namespace fv {
class ActivityState;
}

namespace fv::dictation {

class RecorderPort;
class TranscriptionRuntime;

// The recording a walkthrough is, as the walkthrough runtime reaches it
// (services/runtime/walkthrough.py's calls into the controller, the transcription runtime and the
// recorder): its start and stop, the user's Cancel, the AI write-up still running, pausing, the
// status line and what the recording overlay says is being recorded.
// Affinity: UI actor, as the port; the state queries are thread-safe.
class WalkthroughRecording final : public walkthrough::RecordingPort {
public:
    // `runtime`, `recorder` (the runtime's own) and `activity` must outlive it. `cancel` is the
    // controller's Cancel (the CancelRouter: the recording or job, then the write-up, then the
    // actions still running); empty asks the runtime, then its write-up.
    WalkthroughRecording(TranscriptionRuntime& runtime, RecorderPort& recorder,
                         ActivityState& activity, std::function<void()> cancel = {});

    bool start_walkthrough(std::shared_ptr<walkthrough::WalkthroughSession> session,
                           std::string source) override;
    void stop_recording() override;
    void cancel() override;
    std::shared_ptr<walkthrough::WalkthroughSession> recording_walkthrough() const override;
    bool writing_up() const override;
    bool cancel_write_up() override;
    bool canceling_recording() const override;
    bool capture_canceled() const override;
    bool is_recording() const override;
    bool paused() const override;
    bool pause() override;
    void resume() override;
    double recording_duration() const override;
    // _refuse_dictation_during_meeting: a backup being prepared, the meeting setup open or a
    // meeting running refuses, with its reason on the status line.
    bool refuse_during_meeting() override;
    std::string busy_message() const override;
    void report_status(const std::string& text) override;
    void set_destination_label(const std::string& label) override;

private:
    TranscriptionRuntime& runtime_;
    RecorderPort& recorder_;
    ActivityState& activity_;
    std::function<void()> cancel_;
};

} // namespace fv::dictation
