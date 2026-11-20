#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "ui_personalize/rule_dictation.h"

namespace fv {

class ActivityState;

namespace speech {
class SpeechService;
}

// What a rule dictation's transcription reaches. `activity` and `speech` are required and must
// outlive the transcriber; `busy` is ApplicationController::is_transcribing (false when empty).
struct RuleTranscriberPorts {
    ActivityState* activity = nullptr;
    speech::SpeechService* speech = nullptr;
    std::function<bool()> busy;
};

// application_controller.py transcribe_clip: the Learned rules page's dictated rule, transcribed
// by the dictation engine outside the recording flow. It refuses, in this order, while a backup or
// restore is being prepared ("Wait for backup or restore preparation to finish"), while a paired
// host is preparing an update ("<product> is preparing an update. Try again shortly."), during a
// meeting ("Meeting Mode is active — end the meeting to use dictation"), while the dictation slot
// has no engine loaded ("No transcription engine is available") and while a job holds the engine
// ("Transcription engine is busy"). The clip goes through the speech service, never a Recognizer
// of its own: whole to an engine that takes files, else window by window on the Final lane, in the
// engine's own language.
// Affinity: thread-safe; transcribeClip blocks and runs on the jobs strand, never the GUI thread.
class ControllerRuleTranscriber final : public RuleTranscriber {
public:
    explicit ControllerRuleTranscriber(RuleTranscriberPorts ports);

    std::string transcribeClip(const std::filesystem::path& audio) override;

private:
    RuleTranscriberPorts ports_;
};

} // namespace fv
