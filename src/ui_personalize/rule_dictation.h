#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace fv {

class Recorder;

// A dictated rule stops recording on its own after this long (RULE_DICTATION_CAP_MS).
inline constexpr int kRuleDictationCapMs = 60'000;

// Turns a rule dictation's saved clip into text (application_controller.py transcribe_clip,
// which the Learned rules page reaches as on_dictation_transcribe). The controller provides one
// with ctx.provide<RuleTranscriber>(); without it the page says "Dictation is unavailable.".
// Affinity: thread-safe. transcribeClip blocks, runs on the jobs strand and never on the GUI
// thread; it throws with the sentence the page shows when the clip cannot be transcribed.
class RuleTranscriber {
public:
    virtual ~RuleTranscriber() = default;
    // The words of the WAV at `audio` ("" when nothing was heard).
    virtual std::string transcribeClip(const std::filesystem::path& audio) = 0;

protected:
    RuleTranscriber() = default;
    RuleTranscriber(const RuleTranscriber&) = default;
    RuleTranscriber& operator=(const RuleTranscriber&) = default;
};

// Where a rule dictation's audio is saved while it is transcribed:
// <temp>/<product in lower case>_rule_dictation.wav.
std::filesystem::path ruleDictationPath();

// _rule_capture_problem: why a stopped rule dictation cannot be transcribed, or nullopt once
// its audio is saved at `audio`. Blocking (waits for the recorder to stop, writes the file).
std::optional<std::string> ruleCaptureProblem(Recorder& recorder,
                                              const std::filesystem::path& audio);

// _remove_dictation_audio: deletes the clip, ignoring every failure.
void removeDictationAudio(const std::filesystem::path& audio);

// _transcribe_rule_dictation: stops, saves and transcribes a rule dictation; returns
// (text, error), exactly one of them non-empty. The recording and its audio file are discarded
// either way. A failure logs its exception type only (it could quote the rule) and its text is
// the error ("Transcription failed." when it has none). Blocking; runs on a worker.
std::pair<std::string, std::string>
transcribeRuleDictation(Recorder& recorder,
                        const std::function<std::string(const std::filesystem::path&)>& transcribe,
                        const std::filesystem::path& audio);

} // namespace fv
