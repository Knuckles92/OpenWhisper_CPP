#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "base/subscription.h"
#include "models/catalog.h"
#include "models/hf_access.h"
#include "ui_downloads/downloads_page.h"

class QDialog;
class QProgressDialog;
class QWidget;

namespace fv {

class Executor;
class RecordWidgets;
class SettingsDialog;
class SettingsStore;
class SettingsWindow;

namespace models {
class ComponentRuntime;
class ModelRuntime;
} // namespace models

// What the presenter reaches besides the settings. Every member is optional: what is missing is
// skipped (the runtimes are constructed by the controller; a test leaves out what it does not
// use). Each must outlive the presenter.
struct DownloadsPresenterParts {
    models::ModelRuntime* models = nullptr;
    models::ComponentRuntime* components = nullptr;
    SettingsWindow* settingsWindow = nullptr;
    RecordWidgets* record = nullptr;
};

// UIController's download, delete, batch and component hooks (ui_qt/ui_controller.py:
// on_model_download_*, on_model_batch_*, on_model_deleted, on_component_*,
// refresh_model_views, show_hf_consent_dialog, show_required_runtime_dialog,
// show_gpu_runtime_dialog and the fallback "Downloading model" progress dialog), and the
// Downloads page's requests back to the runtimes.
//
// Model and component events fan out to the Downloads page of the Settings window, its Voice
// model page, Quick Record and Upload File (setModelDownloading), and to the guided setup
// through the signals below. While neither Settings nor the guided setup shows a download, a
// modal-looking "Downloading model" progress dialog does. The runtimes' questions (download
// consent, the required runtime, the GPU runtime offer) are answered through the dialogs of this
// library, parented to the main window.
//
// Provided by DownloadsFeature (ctx.service<DownloadsPresenter>()); the runtimes are attached in
// its install, after every feature provided its services.
// Affinity: GUI thread; the runtimes' signals are delivered on the UI executor.
class DownloadsPresenter : public QObject {
    Q_OBJECT
public:
    // `ui` is the executor the runtimes' signals are delivered on (ctx.ui()); `io` runs the
    // pages' scans (ctx.io()). Both outlive the presenter.
    DownloadsPresenter(SettingsStore& settings, Executor& ui, Executor& io,
                       QObject* parent = nullptr);
    ~DownloadsPresenter() override;

    // Connects the runtimes' signals and the Settings window. Called once.
    void attach(DownloadsPresenterParts parts);
    const DownloadsPresenterParts& parts() const { return parts_; }

    // What a Downloads page reads of this computer (each page built gets a fresh copy).
    void setPageOptions(std::function<DownloadsPageOptions()> options) {
        pageOptions_ = std::move(options);
    }
    DownloadsPageOptions pageOptions() const;

    // The guided setup's shows_model_download(model) while it is visible: it shows that
    // download's progress itself (set by the controller).
    void setDownloadShownElsewhere(std::function<bool(const QString&)> shown) {
        downloadShownElsewhere_ = std::move(shown);
    }

    // The Downloads page of the Settings window, built or deferred; null until one exists. The
    // page is made, deferred, as soon as the window is, so the window's rail can read it before
    // the page is first opened.
    DownloadsPage* page() const { return page_; }
    // The page `dialog`'s Downloads destination shows: the deferred one made with the dialog,
    // reparented into `parent` and built, or a new one. Connects its requests.
    DownloadsPage* pageFor(SettingsDialog* dialog, QWidget* parent);

    // ---- the runtimes' events (public: the controller and tests drive them) ----
    void onModelDownloadStarted(const QString& modelName);
    void onModelDownloadProgress(const QString& modelName, std::int64_t done, std::int64_t total);
    void onModelDownloadFinished(const QString& modelName, bool success);
    void onModelBatchPlanned(const QStringList& modelNames);
    void onModelBatchFinished(int completed, int planned);
    void onModelDeleted(const QString& modelName, bool success, const QString& error);
    void onComponentProgress(const QString& componentId, const QString& phase, std::int64_t done,
                             std::int64_t total);
    void onComponentStateChanged();
    void onComponentInstallFinished(const QString& componentId, bool success,
                                    const QString& message);
    // refresh_model_views: re-reads the Voice model and Downloads pages while Settings shows.
    void refreshModelViews();

    // ---- the runtimes' questions ----
    // show_hf_consent_dialog: the choice, which the caller hands back to ModelRuntime.
    models::ConsentAction askConsent(const QString& modelName, bool envBlocked);
    // show_required_runtime_dialog: install the runtime now.
    bool askRequiredRuntime(const QString& modelName, const QString& componentId);
    // show_gpu_runtime_dialog: the runtime chosen, or nullopt for Later.
    std::optional<std::string> askGpuRuntime(const QString& modelName, const QString& gpuComponent,
                                             const QString& cpuComponent);

    // The fallback progress dialog while it shows; null otherwise.
    QProgressDialog* progressDialog() const { return progressDialog_; }

    // This computer, for the runtime dialogs' sizes and the GPU offer's card (Host::current over
    // the component store, in the app).
    std::function<models::Host()> host;
    // The window the dialogs are parented to (the main window); null parents nothing.
    std::function<QWidget*()> dialogParent;
    // QDialog::exec for the consent and runtime dialogs; a test replaces it.
    std::function<int(QDialog&)> runDialog;

Q_SIGNALS:
    // For the guided setup (model_download_*, runtime_install_*).
    void modelDownloadStarted(const QString& modelName);
    void modelDownloadProgress(const QString& modelName, qint64 done, qint64 total);
    void modelDownloadFinished(const QString& modelName, bool success);
    void componentProgress(const QString& componentId, qint64 done, qint64 total);
    void componentInstallFinished(const QString& componentId, bool success, const QString& message);

private:
    void connectPage(DownloadsPage* page, SettingsDialog* dialog);
    void onDialogCreated(SettingsDialog* dialog);
    SettingsDialog* settingsDialog() const;
    bool settingsVisible() const;
    void setTabsDownloading(const QString& modelName, bool downloading);
    void refreshVoicePage();
    void showDownloadProgress(const QString& modelName);
    void updateDownloadProgress(const QString& modelName, std::int64_t done, std::int64_t total);
    void hideDownloadProgress();
    QWidget* parentWidget() const;
    int run(QDialog& dialog);

    SettingsStore& settings_;
    Executor& ui_;
    Executor& io_;
    DownloadsPresenterParts parts_;
    std::function<DownloadsPageOptions()> pageOptions_;
    std::function<bool(const QString&)> downloadShownElsewhere_;
    QPointer<DownloadsPage> page_;
    // The window page_ belongs to.
    QPointer<SettingsDialog> pageDialog_;
    QPointer<QProgressDialog> progressDialog_;
    std::vector<Subscription> links_;
};

} // namespace fv
