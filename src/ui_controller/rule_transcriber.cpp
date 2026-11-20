#include "ui_controller/rule_transcriber.h"

#include <stdexcept>
#include <utility>

#include "appcore/activity.h"
#include "base/identity.h"
#include "speech/file_windows.h"
#include "speech/service.h"

namespace fv {

namespace {

void required(const void* part, const char* name) {
    if (part == nullptr) {
        throw std::invalid_argument(std::string("The rule transcriber needs ") + name + ".");
    }
}

// A slot with an engine to decode on: one that is loaded, or loaded and still warming.
bool engineLoaded(const speech::Readiness& readiness) {
    return readiness.state == speech::ReadyState::Ready ||
           readiness.state == speech::ReadyState::Warming;
}

} // namespace

ControllerRuleTranscriber::ControllerRuleTranscriber(RuleTranscriberPorts ports)
    : ports_(std::move(ports)) {
    required(ports_.activity, "the activity state");
    required(ports_.speech, "the speech service");
}

// transcribe_clip (application_controller.py:1163-1195).
std::string ControllerRuleTranscriber::transcribeClip(const std::filesystem::path& audio) {
    const ActivitySnapshot activity = ports_.activity->snapshot();
    if (activity.backup_in_progress) {
        throw std::runtime_error("Wait for backup or restore preparation to finish");
    }
    if (activity.host_update_active) {
        throw std::runtime_error(std::string(identity().product) +
                                 " is preparing an update. Try again shortly.");
    }
    if (activity.meeting_active) {
        // The meeting owns the microphone and its engine; a second capture and decode would
        // fight it for the device and the GPU.
        throw std::runtime_error("Meeting Mode is active — end the meeting to use dictation");
    }
    speech::SpeechService& speech = *ports_.speech;
    if (!engineLoaded(speech.readiness(speech::Slot::Dictation))) {
        throw std::runtime_error("No transcription engine is available");
    }
    if (ports_.busy && ports_.busy()) {
        throw std::runtime_error("Transcription engine is busy");
    }
    if (speech.accepts_files(speech::Slot::Dictation)) {
        speech::DecodeJob job;
        job.input = audio;
        job.lane = speech::Lane::Final;
        return speech.run(std::move(job)).transcript.text;
    }
    return speech::decode_file(speech, audio, RecognitionOptions{}, speech::Lane::Final)
        .transcript.text;
}

} // namespace fv
