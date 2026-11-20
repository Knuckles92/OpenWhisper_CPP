#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <QDialog>
#include <QString>

#include "base/cancel.h"
#include "meeting/setup_check.h"

namespace fv {

class Button;
class Executor;
class SettingsStore;
class SuccessButton;
class WrappedLabel;

namespace audio {
class Backend;
}

// Meeting Mode's first-run setup (ui_qt/dialogs/meeting_setup.py MeetingSetupDialog): checks what
// the meeting needs on this computer as it opens, downloads what is missing on request, and
// listens to the microphone and system audio for a moment. Nothing it hears is kept.
//
// Each step runs off the GUI thread; its progress lines and its result come back queued, through
// a mailbox the dialog closes when it is destroyed, so a step that outlives the dialog (a
// download) reaches nothing. While a step runs every action waits. "Start anyway" (starting only)
// is enabled once an audio check has run and the meeting is not fully ready; Done or Start
// Meeting only when the models and the audio are both ready, and it saves meeting_setup_complete
// (a failed save is only logged). Closing during an audio check cancels it and keeps the dialog
// open, disabled, until the check has released the devices; only then does it finish with the
// result asked for.
// Affinity: GUI thread.
class MeetingSetupDialog : public QDialog {
    Q_OBJECT
public:
    // The seams of the steps, each with the production function as its default.
    struct Hooks {
        std::function<meeting::ModelSetup(const meeting::SetupDeps&)> inspect;
        std::function<meeting::ModelSetup(const meeting::SetupDeps&, const CancelToken&,
                                          const std::function<void(std::string)>& status)>
            prepare;
        // meeting::check_audio over the dialog's backend, with its default duration.
        std::function<meeting::AudioCheck(audio::Backend&, const meeting::SetupDeps&,
                                          const CancelToken&, bool systemAudioAllowed)>
            checkAudio;
        // Where meeting_setup_complete is saved; what it throws is logged. Unset: `settings`.
        std::function<void()> markComplete;
        SettingsStore* settings = nullptr;
        // Where the steps run; a thread of their own each when unset. It must outlive every step
        // the dialog started.
        Executor* worker = nullptr;
    };

    // `deps` and `backend` must outlive every step the dialog starts (a download may finish
    // after the dialog is gone). `starting`: opened on the way to a meeting ("Back", "Start
    // anyway" and "Start Meeting"); else from Settings ("Close" and "Done").
    // Two constructors, not a default argument: GCC cannot value-initialise Hooks, whose
    // members have initialisers, before the enclosing class is complete.
    MeetingSetupDialog(meeting::SetupDeps deps, audio::Backend& backend, bool starting,
                       QWidget* parent = nullptr);
    MeetingSetupDialog(meeting::SetupDeps deps, audio::Backend& backend, bool starting,
                       QWidget* parent, Hooks hooks);
    ~MeetingSetupDialog() override;

    // How the meeting should treat system audio: "disabled" after the permission was refused,
    // "required" on Linux once system audio was heard, else "auto".
    QString systemAudioPolicy() const;

    // Whether a step is running.
    bool isBusy() const { return busy_; }

    WrappedLabel* modelStatus() const { return modelStatus_; }
    Button* prepareButton() const { return prepareButton_; }
    WrappedLabel* audioStatus() const { return audioStatus_; }
    Button* audioButton() const { return audioButton_; }
    Button* audioSettingsButton() const { return audioSettings_; }
    Button* cancelButton() const { return back_; }
    Button* skipButton() const { return skip_; }
    SuccessButton* finishButton() const { return finish_; }

public Q_SLOTS:
    // Cancels the running step; during an audio check the result waits for the devices.
    void done(int result) override;

Q_SIGNALS:
    // Setup was finished with everything ready (after meeting_setup_complete was saved, or its
    // save failed), right before the dialog is accepted.
    void setupCompleted();

private:
    struct Mailbox;
    using Done = std::function<void(MeetingSetupDialog&)>;

    static WrappedLabel* label(const QString& text);
    void run(std::function<Done(const CancelToken&, const std::function<void(std::string)>& status)>
                 work,
             WrappedLabel* statusLabel);
    void onStepFinished(const Done& apply, const std::optional<std::string>& error,
                        WrappedLabel* statusLabel);
    void prepare();
    void showModels(const meeting::ModelSetup& models);
    void checkAudio();
    void showAudio(const meeting::AudioCheck& audio);
    void updateButtons();
    void finish();

    meeting::SetupDeps deps_;
    audio::Backend& backend_;
    Hooks hooks_;
    CancelSource cancel_;
    std::shared_ptr<Mailbox> mailbox_;

    bool modelsReady_ = false;
    bool audioReady_ = false;
    bool checkedAudio_ = false;
    bool permissionMissing_ = false;
    bool systemAudioVerified_ = false;
    bool busy_ = false;
    bool checkingAudio_ = false;
    std::optional<int> pendingResult_;
    // The dialog has finished: a step that ends afterwards changes nothing.
    bool closed_ = false;

    WrappedLabel* modelStatus_ = nullptr;
    Button* prepareButton_ = nullptr;
    WrappedLabel* audioStatus_ = nullptr;
    Button* audioButton_ = nullptr;
    Button* audioSettings_ = nullptr;
    Button* back_ = nullptr;
    Button* skip_ = nullptr;
    SuccessButton* finish_ = nullptr;
};

} // namespace fv
