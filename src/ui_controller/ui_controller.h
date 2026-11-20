#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include "appcore/overlay_state.h"
#include "base/subscription.h"
#include "dictation/transcription_runtime.h"
#include "remote/link.h"
#include "ui_controller/quit_confirm.h"
#include "ui_record/transcription_tab_base.h"

class QMenu;

namespace fv {

class ActivityState;
class ApplicationController;
class ClipboardService;
class CompactRecordController;
class DownloadsPresenter;
class EngineSetup;
class Executor;
class GuidedSetup;
class HistoryPanel;
class HistoryStore;
class MainWindow;
class OverlayRouter;
class QuickRecordTab;
class Recorder;
class RecordWidgets;
class ScratchpadService;
class ScreenRegistry;
class SettingsStore;
class SettingsWindow;
class SystemTray;
class TranscriptionTabBase;
class UploadFileTab;
class UploadPresenter;
class WaveformOverlay;

namespace dictation {
class HotkeyRuntime;
class StreamingRuntime;
} // namespace dictation
namespace models {
class ComponentRuntime;
class ModelRuntime;
} // namespace models

// What the UI controller wires together. Every reference must outlive it; `history` and
// `historyStore` are null when the database did not open, `recorder` in tests without one, and
// each M2 part below `io` when its feature is not linked or could not be built.
struct UiControllerParts {
    MainWindow* window = nullptr;
    ScreenRegistry* screens = nullptr;
    RecordWidgets* record = nullptr;
    WaveformOverlay* overlay = nullptr;
    ApplicationController* app = nullptr;
    dictation::TranscriptionRuntime* transcription = nullptr;
    dictation::HotkeyRuntime* hotkeys = nullptr;
    ClipboardService* clipboard = nullptr;
    ActivityState* activity = nullptr;
    Recorder* recorder = nullptr;
    HistoryPanel* history = nullptr;
    std::shared_ptr<HistoryStore> historyStore;
    EngineSetup* engineSetup = nullptr;
    // ctx.ui(); and io, where the History actions block on the database.
    Executor* ui = nullptr;
    Executor* io = nullptr;

    SettingsStore* settings = nullptr;
    // The Upload File tab's presenter: its jobs' results, stages and statuses go to its card.
    UploadPresenter* upload = nullptr;
    dictation::StreamingRuntime* streaming = nullptr;
    ScratchpadService* scratchpad = nullptr;
    models::ModelRuntime* models = nullptr;
    models::ComponentRuntime* components = nullptr;
    GuidedSetup* guidedSetup = nullptr;
    DownloadsPresenter* downloads = nullptr;
    SettingsWindow* settingsWindow = nullptr;
};

// The shell glue of OpenWhisper's UIController (ui_qt/ui_controller.py): Quick Record, Upload
// File (through UploadPresenter), the compact controller, the main window, the tray, the overlay
// (through OverlayRouter), the language menu, the Scratchpad and the History panel on one side,
// the application controller, the dictation, live preview, model and component runtimes and the
// guided setup on the other. Widgets emit intent and expose setters; this class connects them
// (architecture 8.5).
// Affinity: GUI thread. Runtime signals are connected on the UI executor.
class UiController : public QObject {
    Q_OBJECT
public:
    explicit UiController(UiControllerParts parts, QObject* parent = nullptr);
    ~UiController() override;

    // The tray Bootstrap made after the features installed: its recording item and signals.
    void attachTray(SystemTray& tray);
    OverlayRouter& overlayRouter() const { return *router_; }
    bool isRecording() const { return isRecording_; }

    // ---- what the window, tray and shortcuts ask ----
    // start_recording: false when refused, after the window's recording chrome went back.
    bool startRecording(const QString& source = QStringLiteral("ui"));
    // start_recording(profile_id): a non-empty profile starts through the application
    // controller's profile start (onProfileRecordStart), which skips its readiness check.
    bool startRecording(const QString& source, const QString& profileId);
    // on_profile_record_start: the application controller's start_profile_recording; read at
    // each start. While empty a profile start is a plain one.
    std::function<bool(const std::string& profileId, const std::string& source)>
        onProfileRecordStart;
    void stopRecording();
    void cancelRecording();
    void onRecordToggled(bool recording);
    // The tray's record item (_on_tray_toggle_recording).
    void onTrayToggleRecording(const QString& source = QStringLiteral("ui"));
    void onModelChanged(const QString& displayName);
    void onQuickRecordCopy(const QString& text);
    void copyLastOriginal();
    void pasteLastOriginal();
    void showAbout();

    // ---- what the runtimes tell ----
    void displayTranscript(const QString& text, const std::optional<QString>& raw);
    void displayTranscriptionError(const QString& message);
    void setStatus(const QString& status);
    void setAmbientStatus(const QString& status);
    // Every engine switch and reload reports here, so it also refreshes the Remote backend's
    // Model field from what the host said it runs when it connected.
    void setDeviceInfo(const QString& deviceInfo, std::optional<bool> ready);
    void setEngineBusy(bool busy);

    // ---- the Remote engine (the paired computer) on both record tabs ----
    // The connection to the paired computer, for the engine card (set_remote_link).
    void setRemoteLink(const std::optional<remote::RemoteLink>& link);
    // Shows the paired computer's models from getRemoteModels (refresh_remote_models).
    void refreshRemoteModels();
    // Host: the paired computers connected to this one's engine now (set_remote_clients).
    void setRemoteClients(const std::vector<ServedClient>& clients);
    // The remote runtime's (services/runtime/remote.py) hooks, read at each use: what the
    // Model field shows, a Model field choice, a runtime field choice and a click on an
    // offline link. Unset, the choices do nothing.
    std::function<std::optional<remote::RemoteModels>()> getRemoteModels;
    std::function<void(const std::string& family, const std::string& model)> onRemoteModelSelected;
    std::function<void(const std::string& family, const std::string& model, const Json& changes)>
        onRemoteRuntimeSelected;
    std::function<void()> onRemoteRetry;
    void setTranscriptionStats(const dictation::TranscriptionStats& stats);
    void clearTranscriptionStats();
    void showNotice(const QString& text, bool done);
    void showCopiedAnimation();
    void setHandsFree(bool on);
    void onRecordingDeviceSwitched(const QString& oldName, const QString& newName);
    void onRecordingStateChanged(bool recording);
    // _on_live_preview_changed: a tab's footer saved the Live preview switch; every tab shows
    // it, then the preview is set up again.
    void onLivePreviewChanged();
    // cycle_dictation_language (the shortcut and the overlay's chip).
    void cycleDictationLanguage();
    // populate_language_menu: the tray's Language submenu, just before it opens.
    void populateLanguageMenu(QMenu* menu);
    // update_hotkey_display: the record, cancel, enable/disable, minimize, walkthrough and
    // screenshot keys as the window, Quick Record and the compact controller show them.
    void updateHotkeyDisplay(const std::map<std::string, std::string>& hotkeys);
    void refreshHistory();
    // on_dictation_started: the recording's results are Quick Record's, and its profile card
    // shows the profile the recording formats with ("" for none).
    void onDictationStarted(const QString& profileId);
    // on_dictionary_term_learned: 'AI cleanup will keep "<term>" as you said it' for a word kept
    // from cleanup, else 'Added "<term>" to your dictionary'; the Dictionary page re-reads.
    void onDictionaryTermLearned(const QString& term, bool protectedTerm);

    // ---- AI cleanup and profiles (the Settings window's callbacks) ----
    // refresh_cleanup_controls: Quick Record's cleanup controls, and Upload File's once built.
    void refreshCleanupControls();
    // open_cleanup_profiles: Settings on its Profiles page (Quick Record's Manage).
    void openCleanupProfiles();
    // _on_cleanup_profiles_changed: Quick Record lists the profiles again, then
    // onCleanupProfilesChangedHook (the profile shortcuts) runs.
    void onCleanupProfilesChanged();
    std::function<void()> onCleanupProfilesChangedHook;
    // _on_profile_hotkey_capture: the global shortcuts pause while a page captures one.
    void onProfileHotkeyCapture(bool suspended);
    std::function<void(bool suspended)> onProfileHotkeyCaptureHook;

    // _on_settings_page_changed: a Settings page saved something of `kind` (the Settings window,
    // M2, calls it). onSettingsChanged is read at each call, so a handler assigned later is the
    // one called; it starts as the application controller's.
    void settingsPageChanged(const QString& kind);
    std::function<void(std::string_view kind)> onSettingsChanged;

    // _confirm_quit_while_busy's prompt; tests answer it.
    QuitPrompt quitPrompt;
    // Shows the About box; tests replace it.
    std::function<void()> aboutBox;

    // UIController.cleanup: the clipboard back, the overlay's timers stopped and closed.
    void cleanup();

    // The Quick Record tab once the window built it.
    QuickRecordTab* quickRecord() const;
    CompactRecordController* compactController() const;

private:
    void wireWindow();
    void wireQuickRecord(QuickRecordTab* tab);
    void wireCompact();
    void wireRuntimes();
    void wireM2Runtimes();
    void wireHistory();
    void wireTranscriptionTab(TranscriptionTabBase* tab);
    void wireUpload(UploadFileTab* tab);
    void revertRefusedRecordStart();
    void applyToQuickRecord(std::string method, std::function<void(QuickRecordTab*)> apply);
    template <class Fn> void onUi(Fn&& fn);

    UiControllerParts parts_;
    OverlayRouter* router_;
    QPointer<SystemTray> tray_;
    QPointer<QuickRecordTab> wiredQuickRecord_;
    QPointer<UploadFileTab> wiredUpload_;
    struct LanguageUi;
    std::unique_ptr<LanguageUi> languageUi_;
    bool isRecording_ = false;
    std::vector<Subscription> links_;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    struct HistoryBridge;
    std::shared_ptr<HistoryBridge> historyBridge_;
};

} // namespace fv
