#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <vector>

#include <QMetaMethod>
#include <QString>
#include <QVariantMap>
#include <QWidget>

#include "appcore/overlay_state.h"
#include "remote/link.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/local_engine_controls.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPropertyAnimation;
class QPushButton;
class QScrollArea;
class QTimer;
class QTextEdit;
class QVBoxLayout;

namespace fv {

class DownloadsLabel;
class HeaderCard;
class RemoteEngineControls;
class RemoteLinkGlyph;
class RemoteModelNotice;
class WrappedLabel;
class SettingsStore;
class SpeechServerControls;
class TranscriptionStatsWidget;
class TranscriptPane;

// The knobs a tab sets on the shared scaffolding (OpenWhisper's class attributes): what its
// content is called, what the engine readout says before anything is known, and what the
// transcript pane shows and does.
struct TranscriptionTabKnobs {
    QString contentObjectName = QStringLiteral("transcriptionTabContent");
    QString initialStatus;
    QString transcriptPlaceholder = QStringLiteral("Transcription will appear here...");
    // Show the Live preview checkbox in the engine footer. The preview only runs while
    // dictating, so tabs without a microphone flow turn it off.
    bool livePreviewControl = true;
    // Dictation here uses "Languages I dictate in" once they are set, so the engine's Language
    // field steps aside; files keep the engine's language (DICTATION_LANGUAGES).
    bool dictationLanguages = true;
    // Render the transcript as Markdown. Off for dictation, whose cleanup returns prose; on
    // where the transcript carries structure of its own.
    bool transcriptMarkdown = false;
    int transcriptMinHeight = 130;
    // Builds the transcript's text edit; a plain QTextEdit when unset.
    std::function<QTextEdit*()> makeTranscriptEditor;
};

// Why the engine behind a selected_model value cannot show a live preview, or "" when it can.
// Parakeet previews by decoding windows of the recording; the engines that take audio elsewhere
// have no draft text to show.
QString livePreviewUnavailableReason(const QString& backend);

// One paired computer this computer's engine serves (connected_clients() rows): its name and
// whether a request of its is running.
struct ServedClient {
    QString name;
    bool busy = false;
};

// The Remote backend's Model field help (_REMOTE_MODEL_HELP).
extern const QString kRemoteModelHelp;

// Shared model-selection and transcript tab scaffolding: the engine card (backend, local engine
// fields, AI cleanup and Live preview checkboxes, status dot and readout), room for tab-specific
// content, and the collapsible Transcription card with its Fixed / Raw switch and stats strip.
// A subclass overrides the build hooks and calls initialize() as the last step of its own
// constructor: the hooks are virtual, and a base constructor would not reach them.
// Affinity: GUI thread.
class TranscriptionTabBase : public QWidget {
    Q_OBJECT
public:
    // The collapse stretch is applied after the card's animation, so it follows it.
    static constexpr int kCollapseStretchDelayMs = kCollapseDurationMs;

    TranscriptionTabBase(SettingsStore& settings, LocalEngineCatalog catalog,
                         TranscriptionTabKnobs knobs, QWidget* parent = nullptr);

    // ---- backend ----

    // The selected backend's display name (config.MODEL_CHOICES label).
    const QString& currentBackend() const { return currentModel_; }
    // Selects a backend and announces it, as if the user picked it. Choosing API again refreshes
    // its fields and announces it again (the server or model may have changed).
    void chooseBackend(const QString& displayName);
    // Shows a backend as selected without emitting modelChanged.
    virtual void setBackend(const QString& displayName);
    // Shows the saved engine: its backend and that backend's fields. Runs when a save changes the
    // engine, whoever made it, so every tab and Settings agree without telling each other.
    void syncEngineSelection();
    // Selects a backend by its internal value (for example "parakeet").
    void setModelSelection(const QString& modelValue);
    // Locks the backend choice, for example while recording: the combo, the API server's fields
    // and the Remote backend's Model and runtime fields.
    void setBackendEnabled(bool enabled);

    // ---- API backend ----

    // Takes the API backend's Server and Model fields into the field row, after the local
    // engine's (stretch 4), shown while the API backend is selected. A tab without them still
    // lists the API backend, with no fields of its own. Call once, right after construction:
    // building them needs the HTTP client and the io executor, which the feature has and the tab
    // does not.
    void setApiServer(SpeechServerControls* controls);
    SpeechServerControls* apiServer() const { return apiServer_; }
    // Shows the saved API server and model again.
    void refreshApiModel();
    // Switches between the local runtime fields and the API server's fields. The status dot goes
    // with the local fields: the API has no engine here for it to report on.
    void setLocalEngineVisible(bool visible);

    // ---- engine readout ----

    void setEngineMessage(const QString& statusText);
    // Shows statusText only where the idle readout would be: for news nobody asked for, such as
    // a failed automatic update check, which must not replace a message that explains a problem.
    void setAmbientMessage(const QString& statusText);
    // Updates the idle readout and readiness from a completed engine probe; ready is unknown
    // until the probe says.
    void setDeviceInfo(const QString& deviceInfo, std::optional<bool> ready = std::nullopt);
    void setEngineBusy(bool busy);
    bool engineLoading() const { return engineBusy_ || !modelDownloads_.empty(); }
    void setModelDownloading(const QString& modelName, bool downloading);
    // Drives activity feedback from the same explicit state as the overlay.
    void setActivityState(appcore::OverlayState state);

    // ---- cleanup and live preview ----

    // Loads the AI cleanup checkbox from settings without writing or emitting.
    virtual void loadCleanupSetting();
    // Loads the saved Live preview choice and shows it.
    void loadLivePreviewSetting();
    // Locks the toggle, for example while recording, without unlocking it early.
    void setLivePreviewLocked(bool locked);
    QString livePreviewUnavailableReason() const;

    // ---- transcript ----

    // Displays fixed text and an optional distinct raw ASR version: the raw text is kept only
    // when it is not empty and differs from text.
    virtual void setTranscript(const QString& text,
                               const std::optional<QString>& raw = std::nullopt);
    virtual void clearTranscription();
    void redrawTranscript();
    // The source text of the version currently displayed (Fixed or Raw).
    QString shownTranscript() const;
    bool isTranscriptionCollapsed() const;
    // An open transcript takes whatever height the window is given.
    bool fillsHeight() const { return !isTranscriptionCollapsed(); }
    // Expands the transcript card through the normal user-toggle path.
    void expandTranscription();
    // cleanupTime is accepted for parity with the runtime's report; the strip does not show it.
    // remote names the paired computer that did the work.
    virtual void setTranscriptionStats(double transcriptionTime, double audioDuration,
                                       std::optional<std::int64_t> fileSize,
                                       double cleanupTime = 0.0,
                                       const std::optional<remote::RemoteTiming>& remote = {});

    // ---- Remote backend (the paired computer) ----

    // The connection to the paired computer changed. While Remote is selected the link glyph
    // stands in for the status dot and the readout says how the link is; offline with a retry
    // scheduled, it counts down to it every second.
    void setRemoteLink(const std::optional<remote::RemoteLink>& link);
    // What the paired computer can run, and what it runs now. Before the first report the Model
    // field names the host of the saved pairing.
    void setRemoteModels(const std::optional<remote::RemoteModels>& choices);
    // Rebuilds the Model field from the last report (after a refused switch, say).
    void refreshRemoteModels() { showRemoteModels(); }
    // Host side: the paired computers this computer's engine serves now.
    void setRemoteClients(const std::vector<ServedClient>& clients);
    // The readout's text for the link ("Connected to jed · Tailscale · 24 ms"); "" without one.
    QString remoteLinkText() const;
    QString remoteLinkTooltip() const;
    virtual void clearTranscriptionStats();

    // ---- parts the tabs and tests reach ----

    QScrollArea* scrollArea() const { return scrollArea_; }
    QVBoxLayout* contentLayout() const { return contentLayout_; }
    QWidget* engineCard() const { return engineCard_; }
    StatusFieldCombo* modelCombo() const { return modelCombo_; }
    LocalEngineControls* localEngine() const { return localEngine_; }
    StatusDot* statusDot() const { return statusDot_; }
    DownloadsLabel* resolvedLabel() const { return resolvedLabel_; }
    QCheckBox* cleanupCheck() const { return cleanupCheck_; }
    QCheckBox* livePreviewCheck() const { return livePreviewCheck_; }
    HeaderCard* transcriptionCard() const { return transcriptionCard_; }
    TranscriptPane* transcriptPane() const { return transcriptPane_; }
    QTextEdit* transcriptText() const { return transcriptText_; }
    QWidget* versionToggle() const { return versionToggle_; }
    QPushButton* fixedButton() const { return fixedButton_; }
    QPushButton* rawButton() const { return rawButton_; }
    TranscriptionStatsWidget* statsWidget() const { return statsWidget_; }
    QComboBox* remoteModelCombo() const { return remoteModelCombo_; }
    QWidget* remoteModelField() const { return remoteModelField_; }
    RemoteEngineControls* remoteEngine() const { return remoteEngine_; }
    WrappedLabel* remoteRuntimeLabel() const { return remoteRuntimeLabel_; }
    RemoteModelNotice* remoteManageButton() const { return manageHostModels_; }
    RemoteLinkGlyph* linkGlyph() const { return linkGlyph_; }
    // Ticks the "Trying again in N s." countdown, only while one is shown.
    QTimer* linkCountdown() const { return linkCountdown_; }
    QWidget* servingRow() const { return servingRow_; }
    QLabel* servingLabel() const { return servingLabel_; }
    RemoteLinkGlyph* servingGlyph() const { return servingGlyph_; }
    SettingsStore& settings() const { return settings_; }
    const TranscriptionTabKnobs& knobs() const { return knobs_; }
    // Whether anything is connected to signal ("connects every signal" tests).
    bool hasReceivers(const QMetaMethod& signal) const { return isSignalConnected(signal); }

    // Slots, because the main window calls them by name (setStatus(QString),
    // setTranscriptionCollapsed(bool)) on pages built by later features (lead amendment 21).
public Q_SLOTS:
    virtual void setStatus(const QString& statusText);
    // Applies the collapsed state without persisting or emitting (sync and restore).
    virtual void setTranscriptionCollapsed(bool collapsed);

Q_SIGNALS:
    // The display name of the backend the person chose, or an engine setting of it changed.
    void modelChanged(const QString& displayName);
    // A local engine field changed and was saved.
    void engineSettingsChanged();
    // A help link was followed: the Settings destination to open.
    void helpRequested(const QString& destination);
    void engineDownloadsRequested();
    // collapsed, and the height the card's body gave up.
    void transcriptionCollapsed(bool collapsed, int freedHeight);
    // The Live preview checkbox persisted a change.
    void livePreviewChanged();
    // The AI cleanup checkbox persisted a change.
    void cleanupToggled();
    // The person chose one of the paired computer's models: (family, model).
    void remoteModelSelected(const QString& family, const QString& model);
    // The person changed a runtime setting of the paired computer: (family, model, {key: value}).
    void remoteRuntimeSelected(const QString& family, const QString& model,
                               const QVariantMap& changes);
    // The link was clicked while offline: reconnect now.
    void remoteRetryRequested();

protected:
    // Builds the scaffolding, calls the hooks, connects and loads the saved settings.
    void initialize();

    // Inserts tab-specific widgets after the engine card.
    virtual void buildContentBeforeStatus(QVBoxLayout* layout);
    // Inserts tab-specific widgets before the transcription card.
    virtual void buildContentAfterStatus(QVBoxLayout* layout);
    virtual void connectSignals();
    // The person chose another backend in the combo: shown, its fields refreshed, announced.
    virtual void onBackendChanged();

private:
    void setupUi();
    void onCleanupToggled(bool checked);
    void onLivePreviewToggled(bool checked);
    void syncLivePreview();
    void onVersionToggled(bool checked);
    void showTranscriptText(const QString& text);
    void onApiServerChanged();
    void refreshEngineStatus();
    void applyBackendStatus(EngineStatus status);
    void applyTranscriptionStretch(bool collapsed);
    void onTranscriptionToggled(bool collapsed);
    void showVersionToggle(bool visible);
    int backendIndex(const QString& displayName) const;
    // The paired computer's name while Remote is shown and connected; "" otherwise.
    QString connectedHost() const;
    void showRemoteRuntime();
    void showRemoteModels();
    remote::RemoteModels currentRemoteModels() const;
    void syncRemoteModelEnabled();
    void onRemoteModelActivated(int index);
    void onLinkClicked();
    void showServingRow(bool show);
    QString backendValue(const QString& displayName) const;

    SettingsStore& settings_;
    LocalEngineCatalog catalog_;
    TranscriptionTabKnobs knobs_;

    QString currentModel_;
    QString fixedText_;
    std::optional<QString> rawText_;
    bool showingRaw_ = false;
    QString deviceInfo_;
    EngineStatus engineStatus_ = EngineStatus::Unknown;
    bool engineBusy_ = false;
    std::set<QString> modelDownloads_;
    appcore::OverlayState activityState_ = appcore::OverlayState::None;
    QString statusMessage_;
    // False for a backend with no engine for the dot to report on.
    bool engineDotVisible_ = true;
    bool backendEnabled_ = true;
    // The saved Live preview choice. The checkbox shows it only while the selected engine can
    // preview, and a recording locks it.
    bool livePreviewWanted_ = false;
    bool livePreviewLocked_ = false;
    int bottomStretchIndex_ = 0;
    // The paired computer's models; nullopt until the controller reports them.
    std::optional<remote::RemoteModels> remoteModels_;
    std::vector<remote::HostModel> remoteEntries_;
    bool remoteSelectable_ = false;
    // The Remote backend's connection, shown instead of the status dot while Remote is selected;
    // nullopt until the controller says.
    std::optional<remote::RemoteLink> remoteLink_;
    bool remoteShown_ = false;
    // Host side: requests finished for paired computers, each a packet flying home.
    std::int64_t served_ = 0;
    bool servingBusy_ = false;
    bool servingShown_ = false;

    QScrollArea* scrollArea_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    QWidget* engineCard_ = nullptr;
    StatusFieldCombo* modelCombo_ = nullptr;
    QHBoxLayout* fieldRow_ = nullptr;
    QHBoxLayout* footerRow_ = nullptr;
    LocalEngineControls* localEngine_ = nullptr;
    SpeechServerControls* apiServer_ = nullptr;
    StatusDot* statusDot_ = nullptr;
    DownloadsLabel* resolvedLabel_ = nullptr;
    RemoteModelNotice* manageHostModels_ = nullptr;
    QComboBox* remoteModelCombo_ = nullptr;
    QWidget* remoteModelField_ = nullptr;
    RemoteEngineControls* remoteEngine_ = nullptr;
    WrappedLabel* remoteRuntimeLabel_ = nullptr;
    RemoteLinkGlyph* linkGlyph_ = nullptr;
    QTimer* linkCountdown_ = nullptr;
    QWidget* servingRow_ = nullptr;
    QLabel* servingLabel_ = nullptr;
    RemoteLinkGlyph* servingGlyph_ = nullptr;
    QPropertyAnimation* servingAnim_ = nullptr;
    QCheckBox* cleanupCheck_ = nullptr;
    QCheckBox* livePreviewCheck_ = nullptr;
    HeaderCard* transcriptionCard_ = nullptr;
    TranscriptPane* transcriptPane_ = nullptr;
    QWidget* versionToggle_ = nullptr;
    QPushButton* fixedButton_ = nullptr;
    QPushButton* rawButton_ = nullptr;
    QButtonGroup* versionGroup_ = nullptr;
    QTextEdit* transcriptText_ = nullptr;
    TranscriptionStatsWidget* statsWidget_ = nullptr;
};

} // namespace fv
