#pragma once

#include <memory>
#include <vector>

#include <QObject>
#include <QPointer>

#include "base/subscription.h"

namespace fv {

class AppContext;
class ApplicationController;
class CleanupAgentOffer;
class EnvReader;
class KeyPresence;
class SettingsDialog;
class UiController;

namespace clipboard {
class TemporaryClipboard;
}
namespace focus {
class CaptureService;
class SyntheticKeys;
} // namespace focus
namespace dictation {
class CommandRuntime;
class DictionaryLearner;
class HotkeyRuntime;
class TranscriptionRuntime;
} // namespace dictation
namespace llm {
class TranscriptCleanup;
}

// What the M4 text features are built over (provide()): the runtimes and the clipboard, focus
// capture and synthetic keys the controller made. Every pointer is required and outlives the
// context's services this provides.
struct PersonalizeParts {
    dictation::TranscriptionRuntime* transcription = nullptr;
    dictation::HotkeyRuntime* hotkeys = nullptr;
    ApplicationController* app = nullptr;
    clipboard::TemporaryClipboard* clipboard = nullptr;
    focus::SyntheticKeys* keys = nullptr;
    focus::CaptureService* capture = nullptr;
    // Where keys are read beside the credential store and the .env file; system_env() when null.
    const EnvReader* keyEnvironment = nullptr;
};

// The controller's share of every M4 feature (application_controller.py's TranscriptCleanup,
// CommandRuntime, dictionary learning and transcribe_clip; ui_controller.py's profile, cleanup,
// dictionary and Settings callbacks; guided setup's provider row).
//
// provide() builds the deferred dictation and endpoint-only command clients, the cleaner, the
// dictionary learner and the text preparer over it, Command Mode and transforms
// (dictation::CommandRuntime, the hotkey runtime's command keys and the transcription runtime's
// command completion), and the RuleTranscriber of the Learned rules page; each is detached from
// the runtimes before it is destroyed, and Quit runs the command runtime's cleanup first. The
// cleanup client and Command Mode need the network client and the credential store: a context
// without them (a test of the M1 graph) dictates without AI cleanup, and Command Mode says it is
// not available.
//
// install() connects the profile and transform shortcuts, the AI cleanup settings' signals
// (CleanupSettings), Quick Record's profile start and Manage button, the learned-word notice, the
// Dictionary page's live engine, the profile and transform rows of the Hotkeys page, and the
// guided setup's provider row.
// Affinity: UI actor.
class PersonalizeWiring : public QObject {
    Q_OBJECT
public:
    PersonalizeWiring();
    ~PersonalizeWiring() override;

    void provide(AppContext& context, const PersonalizeParts& parts);
    void install(AppContext& context, UiController& ui);

    // Null in a context without the network client or the credential store.
    dictation::CommandRuntime* commandRuntime() const { return commands_; }
    llm::TranscriptCleanup* cleanup() const { return cleanup_; }
    dictation::DictionaryLearner* learner() const { return learner_; }
    // Whether a key is saved for the guided setup's provider row; null before install() or
    // without the credential store.
    KeyPresence* keyPresence() const { return keyPresence_; }
    // The one-time cleanup agent offer; null before install().
    CleanupAgentOffer* cleanupAgentOffer() const { return offer_; }

private:
    void wireDialog(AppContext& context, SettingsDialog* dialog);

    PersonalizeParts parts_;
    dictation::CommandRuntime* commands_ = nullptr;
    llm::TranscriptCleanup* cleanup_ = nullptr;
    dictation::DictionaryLearner* learner_ = nullptr;
    KeyPresence* keyPresence_ = nullptr;
    CleanupAgentOffer* offer_ = nullptr;
    std::vector<QPointer<SettingsDialog>> wired_;
    std::vector<Subscription> links_;
};

// Whether the Learned rules page of the app's Settings window is recording or transcribing a rule
// (the backup waits for it, as _acquire_backup reads cleanup_rules.dictation_state). GUI thread.
bool ruleDictationBusy(AppContext& context);

} // namespace fv
