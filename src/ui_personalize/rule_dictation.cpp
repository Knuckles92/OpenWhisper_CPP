#include "ui_personalize/rule_dictation.h"

#include <cctype>
#include <exception>
#include <system_error>
#include <typeinfo>

#include "appkit/app_context.h"
#include "base/identity.h"
#include "base/log.h"
#include "recorder/recorder.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_cleanup_rules"};

bool isBlank(const std::string& text) {
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

} // namespace

std::filesystem::path ruleDictationPath() {
    std::string product(identity().product);
    for (char& c : product) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::error_code error;
    std::filesystem::path folder = std::filesystem::temp_directory_path(error);
    if (error) {
        folder = std::filesystem::path(".");
    }
    return folder / (product + "_rule_dictation.wav");
}

std::optional<std::string> ruleCaptureProblem(Recorder& recorder,
                                              const std::filesystem::path& audio) {
    if (!recorder.wait_for_stop_completion()) {
        return std::string("Recording did not finish stopping; audio kept for recovery.");
    }
    if (std::string error = recorder.last_capture_error(); !error.empty()) {
        return error;
    }
    if (!recorder.has_recording_data()) {
        return std::string("No audio was captured.");
    }
    if (!recorder.save_recording(audio)) {
        return std::string("Couldn't save the dictation audio.");
    }
    return std::nullopt;
}

void removeDictationAudio(const std::filesystem::path& audio) {
    std::error_code ignored;
    if (std::filesystem::exists(audio, ignored)) {
        std::filesystem::remove(audio, ignored);
    }
}

std::pair<std::string, std::string>
transcribeRuleDictation(Recorder& recorder,
                        const std::function<std::string(const std::filesystem::path&)>& transcribe,
                        const std::filesystem::path& audio) {
    std::string text;
    std::string error;
    try {
        recorder.stop_recording();
        if (std::optional<std::string> problem = ruleCaptureProblem(recorder, audio)) {
            error = *problem;
        } else {
            text = transcribe ? transcribe(audio) : std::string();
            if (isBlank(text)) {
                error = "Nothing was transcribed.";
            }
        }
    } catch (const std::exception& failure) {
        // Recording and speech-engine code: anything may fail, and the page shows the error.
        // Type only in the log: it could quote the rule.
        kLog.warning("Rule dictation failed ({})", type_display_name(typeid(failure)));
        error = failure.what();
        if (error.empty()) {
            error = "Transcription failed.";
        }
    }
    try {
        // Discarded: the rule's words are not a dictation to recover, so its journal folder
        // must not stay behind (OpenWhisper leaves it, and offers no recovery for it either).
        recorder.clear_recording_data(true);
    } catch (const std::exception& failure) {
        kLog.warning("Couldn't release the rule dictation ({})",
                     type_display_name(typeid(failure)));
    }
    removeDictationAudio(audio);
    if (!error.empty()) {
        text.clear();
    }
    return {text, error};
}

} // namespace fv
