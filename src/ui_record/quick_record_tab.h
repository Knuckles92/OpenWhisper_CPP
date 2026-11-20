#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <QIcon>
#include <QPointer>
#include <QString>
#include <QTimer>

#include "base/json.h"
#include "text/cleanup_profiles.h"
#include "ui_record/transcription_tab_base.h"

class QComboBox;
class QLabel;
class QPushButton;

namespace fv {

class CleanupProfileCard;
class DangerButton;
class SuccessButton;
class WarningButton;
class WrappedLabel;

// The Quick Record tab: the engine card, the cleanup profile card (while AI cleanup is checked),
// the Start Recording / Stop / Cancel buttons with the Command Mode hint under them, and the
// Transcription card whose editor grows with its text (live preview text while recording, the
// finished transcript afterwards). It does not run anything: intent leaves as signals and the
// controller answers through the setters. Affinity: GUI thread.
class QuickRecordTab : public TranscriptionTabBase {
    Q_OBJECT
public:
    // The readiness of the AI provider Command Mode rewrites with: nullopt while that is not
    // known yet (treated as working), false when it is known not to work. The setting snapshot is
    // the argument.
    using CommandModeReadiness = std::function<std::optional<bool>(const Json& settings)>;

    static constexpr int kCopyFeedbackMs = 1500;

    QuickRecordTab(SettingsStore& settings, LocalEngineCatalog catalog, QWidget* parent = nullptr);

    // ---- recording ----

    bool isRecording() const { return isRecording_; }
    // Shows the current recording state in the buttons, the locks and the status.
    void updateRecordingState();

    // Live preview text: a final text replaces the buffer (the preview emits all it has each
    // cycle), a draft is appended after it with a trailing " ...".
    void setPartialTranscription(const QString& text, bool isFinal);
    void clearPartialTranscription();

    // The shortcut texts the buttons show on hover. The enable/disable key is accepted and
    // ignored, as OpenWhisper does.
    void updateHotkeys(const QString& recordKey, const QString& cancelKey,
                       const QString& enableDisableKey = {});

    // ---- copy ----

    // Shows whether the copy the tab asked for worked, for kCopyFeedbackMs.
    void showCopyResult(bool succeeded);

    // ---- cleanup profiles ----

    // The id of the chosen cleanup profile, "" for standard dictation.
    QString selectedCleanupProfileId() const;
    // Lists the saved profiles again and shows the saved choice (quick_record_profile). While
    // recording it shows the profile the recording uses, kept in the list even when it has been
    // deleted since.
    void refreshCleanupProfiles();
    // The profile the recording in progress uses: the saved profile with that id ("" or an id
    // that names none: standard dictation). It is shown, and listed, until the recording ends.
    void setRecordingProfile(const QString& id);
    void setRecordingProfile(std::optional<text::CleanupProfile> profile);

    // ---- command mode ----

    // Replaces the default, which reports the provider as not ready. The supplier runs on the
    // GUI thread at every refresh: it answers from what it knows and does not block.
    void setCommandModeReadiness(CommandModeReadiness readiness);
    // Reads the Command Mode shortcut and the provider's readiness again and shows the hint or
    // the link. Done by itself when the window is activated and when a lookup lands.
    void refreshCommandHint();

    void setTranscriptionCollapsed(bool collapsed) override;
    void loadCleanupSetting() override;

    SuccessButton* recordButton() const { return recordButton_; }
    DangerButton* stopButton() const { return stopButton_; }
    WarningButton* cancelButton() const { return cancelButton_; }
    QPushButton* copyButton() const { return copyButton_; }
    QPushButton* collapsedCopyButton() const { return collapsedCopyButton_; }
    WrappedLabel* commandHint() const { return commandHint_; }
    QPushButton* commandLink() const { return commandLink_; }
    CleanupProfileCard* profileCard() const;
    // Null once the card is gone.
    QComboBox* profileCombo() const;
    WrappedLabel* profileHint() const;

public Q_SLOTS:
    // Sets the recording state and shows it. The main window's recording relay calls it by name
    // (setRecordingActive(bool)), so it is a slot.
    void setRecordingActive(bool recording);
    // An installed agent's lookup finished: its readiness may have changed (the relay of
    // OpenWhisper's agent_lookup.lookup_relay).
    void onAgentLookedUp(const QString& agentId);

Q_SIGNALS:
    // A start (true) or stop (false) was asked for; the recording chrome waits for the answer.
    void recordToggled(bool recording);
    void recordCanceled();
    // The text shown (Fixed or Raw) for the clipboard.
    void copyRequested(const QString& text);
    // Settings destination to open (the Command Mode page).
    void settingsRequested(const QString& destination);
    // The Manage button of the cleanup profile card was pressed: the profile manager is wanted.
    void profilesRequested();

protected:
    bool event(QEvent* event) override;
    void buildContentBeforeStatus(QVBoxLayout* layout) override;
    void buildContentAfterStatus(QVBoxLayout* layout) override;
    void connectSignals() override;

private:
    void syncCopyActions();
    void requestCopy();
    void resetCopyFeedback();
    void refreshProfileHint();
    void onProfileChanged();
    const text::CleanupProfile* profileById(const QString& id) const;
    void onRecordClicked();

    bool isRecording_ = false;
    QStringList partialBuffer_;
    QString standardRecordKey_;
    CommandModeReadiness readiness_;
    std::vector<text::CleanupProfile> cleanupProfiles_;
    // The profile the recording in progress uses, whatever the library says by now.
    std::optional<text::CleanupProfile> activeProfile_;
    // Guarded: the reference screenshots delete the card, as the builder's _prune does.
    QPointer<CleanupProfileCard> profileCard_;

    QIcon copyIcon_;
    QIcon copiedIcon_;
    QPushButton* copyButton_ = nullptr;
    QPushButton* collapsedCopyButton_ = nullptr;
    QTimer* copyFeedbackTimer_ = nullptr;

    QWidget* commandRow_ = nullptr;
    WrappedLabel* commandHint_ = nullptr;
    QPushButton* commandLink_ = nullptr;
    SuccessButton* recordButton_ = nullptr;
    DangerButton* stopButton_ = nullptr;
    WarningButton* cancelButton_ = nullptr;
};

} // namespace fv
