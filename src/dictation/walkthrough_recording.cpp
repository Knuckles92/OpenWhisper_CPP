#include "dictation/walkthrough_recording.h"

#include <utility>

#include "appcore/activity.h"
#include "dictation/recorder_port.h"
#include "dictation/transcription_runtime.h"

namespace fv::dictation {

WalkthroughRecording::WalkthroughRecording(TranscriptionRuntime& runtime, RecorderPort& recorder,
                                           ActivityState& activity, std::function<void()> cancel)
    : runtime_(runtime), recorder_(recorder), activity_(activity), cancel_(std::move(cancel)) {}

bool WalkthroughRecording::start_walkthrough(
    std::shared_ptr<walkthrough::WalkthroughSession> session, std::string source) {
    return runtime_.start_walkthrough(std::move(session), std::move(source));
}

void WalkthroughRecording::stop_recording() {
    runtime_.stop_recording();
}

void WalkthroughRecording::cancel() {
    if (cancel_) {
        cancel_();
        return;
    }
    // The controller's first two steps: the recording or job in progress, then the write-up.
    if (!runtime_.cancel()) {
        runtime_.cancel_write_up();
    }
}

std::shared_ptr<walkthrough::WalkthroughSession>
WalkthroughRecording::recording_walkthrough() const {
    return runtime_.recording_walkthrough();
}

bool WalkthroughRecording::writing_up() const {
    return runtime_.writing_up();
}

bool WalkthroughRecording::cancel_write_up() {
    return runtime_.cancel_write_up();
}

bool WalkthroughRecording::canceling_recording() const {
    return runtime_.canceling_recording();
}

bool WalkthroughRecording::capture_canceled() const {
    return runtime_.capture_canceled();
}

bool WalkthroughRecording::is_recording() const {
    return runtime_.is_recording();
}

bool WalkthroughRecording::paused() const {
    return recorder_.paused();
}

bool WalkthroughRecording::pause() {
    return recorder_.pause();
}

void WalkthroughRecording::resume() {
    recorder_.resume();
}

double WalkthroughRecording::recording_duration() const {
    return recorder_.get_recording_duration();
}

bool WalkthroughRecording::refuse_during_meeting() {
    const auto reason = refuse_dictation(activity_.snapshot());
    if (!reason) {
        return false;
    }
    runtime_.status_update.emit(*reason);
    return true;
}

std::string WalkthroughRecording::busy_message() const {
    return runtime_.readiness_message().value_or(std::string());
}

void WalkthroughRecording::report_status(const std::string& text) {
    runtime_.status_update.emit(text);
}

void WalkthroughRecording::set_destination_label(const std::string& label) {
    runtime_.recording_destination_changed.emit(label);
}

} // namespace fv::dictation
