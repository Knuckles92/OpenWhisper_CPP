#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include "base/json.h"
#include "base/subscription.h"
#include "ui_setup/guided_setup_dialog.h"

class QTimer;
class QWidget;

namespace fv {

class AppContext;
class OpenWhisperImport;
class SettingsStore;

// The app's one guided setup window and its first-run offer (ui_controller.py
// offer_guided_setup, _offer_guided_setup_now, open_guided_setup, _show_guided_setup and
// _download_model_for_setup), provided as a feature service (ctx.service<GuidedSetup>()).
//
// The offer starts kOfferDelayMs after the main window first shows, and only while
// guided_setup_state is "pending". When it fires it checks again; while a modal window is up
// (a download consent, a recovery prompt) or the main window is hidden or minimized it retries
// every kOfferRetryMs. A visible window is never offered again, and a skipped setup never is.
// File > "Guided setup…" opens straight on Record. The window is created once and reused.
// Affinity: GUI thread.
class GuidedSetup : public QObject {
    Q_OBJECT
public:
    static constexpr int kOfferDelayMs = 750;
    static constexpr int kOfferRetryMs = 1000;

    // The seams the controller's checks go through; a test replaces them.
    struct Hooks {
        // QApplication.activeModalWidget() is not None.
        std::function<bool()> modalActive;
        // _raise_dialog: show, raise, activate.
        std::function<void(GuidedSetupDialog&)> raiseDialog;
    };

    // `ports` is asked once, when the window is first created.
    GuidedSetup(SettingsStore& store, std::function<GuidedSetupPorts()> ports,
                QObject* parent = nullptr);
    // Ports from the context's services (portsFor), taken when the window is first created.
    explicit GuidedSetup(AppContext& context);
    ~GuidedSetup() override;

    // The window the setup window belongs to and whose visibility the offer waits for.
    void setMainWindow(QWidget* window);
    // Offers setup once `window` first shows (bootstrap: offer_guided_setup right after
    // show_main_window).
    void watchFirstShow(QWidget* window);
    Hooks& hooks() { return hooks_; }

    // offer_guided_setup: starts the offer timer when setup should invite.
    void offer();
    // _offer_guided_setup_now: the timer's check.
    void offerNow();
    // open_guided_setup: File > Guided setup…, straight to the first step.
    void open();

    // A dictation's result while the window may be on Ready (transcription_received; `pasted`
    // is not read: the try box waits for a paste by the auto-paste setting, as OpenWhisper's
    // does) and a failed one (transcription_error_received).
    void dictationReceived(const QString& text, bool pasted);
    void dictationFailed(const QString& message);

    // setup.isVisible() and shows_model_download, and shows_runtime_install: the app suppresses
    // its own progress and install dialogs for what this window shows itself. showsDownload is
    // also true for the download the visible window asked for and has not yet heard start, so
    // the answer does not depend on which model_download_started handler runs first.
    bool showsDownload(const QString& model) const;
    bool showsRuntimeInstall(const QString& componentId) const;

    // The controller's download events, forwarded to the window once it exists.
    void modelDownloadStarted(const QString& model);
    void modelDownloadProgress(const QString& model, std::int64_t done, std::int64_t total);
    void modelDownloadFinished(const QString& model, bool success);
    void runtimeInstallProgress(const QString& componentId, std::int64_t done, std::int64_t total);
    void runtimeInstallFinished(const QString& componentId, bool success, const QString& message);

    // provider_summary (M4): until set, the Clean up step's provider row stays hidden.
    void setProviderSummary(std::function<ProviderSummary(const Json&)> summary);
    // _download_model_for_setup: the app's model download (on_model_download_requested).
    void setDownloadModel(std::function<void(const QString&)> download);

    // Wires the app: File > Guided setup…, the offer on the main window's first show, Settings'
    // runtime-install suppression, and the model and runtime download events of
    // models::ModelRuntime and models::ComponentRuntime when the context provides them.
    void install(AppContext& context);

    // The window, null until it is first shown.
    GuidedSetupDialog* dialog() const { return dialog_; }
    QTimer* offerTimer() const { return offerTimer_; }

    // The real ports: Settings through SettingsWindow, the microphone test on the saved
    // microphone order (AppPages' preview recorder), speech status through setup_speech.
    static GuidedSetupPorts portsFor(AppContext& context);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void show(bool welcome);

    SettingsStore& store_;
    std::function<GuidedSetupPorts()> ports_;
    Hooks hooks_;
    QPointer<QWidget> mainWindow_;
    QPointer<QWidget> watched_;
    QPointer<GuidedSetupDialog> dialog_;
    QTimer* offerTimer_ = nullptr;
    std::function<ProviderSummary(const Json&)> providerSummary_;
    std::function<void(const QString&)> downloadModel_;
    // The context's OpenWhisper import, asked to look for OpenWhisper's data when the Welcome
    // page shows (install).
    QPointer<OpenWhisperImport> import_;
    bool firstShowSeen_ = false;
    std::vector<Subscription> subscriptions_;
};

} // namespace fv
