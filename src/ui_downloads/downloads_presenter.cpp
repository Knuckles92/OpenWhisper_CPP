#include "ui_downloads/downloads_presenter.h"

#include <exception>
#include <utility>

#include <QDialog>
#include <QProgressDialog>
#include <QVBoxLayout>
#include <QWidget>

#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "history/format_utils.h"
#include "models/component_runtime.h"
#include "models/consent_text.h"
#include "models/hf_access_policy.h"
#include "models/model_runtime.h"
#include "settings/store.h"
#include "ui_downloads/hf_consent_dialog.h"
#include "ui_downloads/required_runtime_dialog.h"
#include "ui_downloads/voice_model_page.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_record/upload_file_tab.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_window.h"
#include "ui_widgets/nav_rail.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string u(const QString& text) {
    return qt::toUtf8(text);
}

QString downloadsKey() {
    return q(destinations::downloads);
}

} // namespace

DownloadsPresenter::DownloadsPresenter(SettingsStore& settings, Executor& ui, Executor& io,
                                       QObject* parent)
    : QObject(parent), runDialog([](QDialog& dialog) { return dialog.exec(); }),
      settings_(settings), ui_(ui), io_(io) {}

DownloadsPresenter::~DownloadsPresenter() {
    links_.clear();
    hideDownloadProgress();
    // A deferred page never placed in a window has no parent to take it.
    if (page_ != nullptr && page_->parent() == nullptr) {
        delete page_.data();
    }
}

DownloadsPageOptions DownloadsPresenter::pageOptions() const {
    return pageOptions_ ? pageOptions_() : DownloadsPageOptions{};
}

void DownloadsPresenter::attach(DownloadsPresenterParts parts) {
    parts_ = parts;
    if (SettingsWindow* window = parts_.settingsWindow) {
        connect(window, &SettingsWindow::dialogCreated, this, &DownloadsPresenter::onDialogCreated);
        if (SettingsDialog* dialog = window->dialog()) {
            onDialogCreated(dialog);
        }
        if (models::ModelRuntime* runtime = parts_.models) {
            // get_missing_local_runtime, for "engine_downloads".
            window->setMissingRuntimeProvider(
                [runtime] { return q(runtime->missing_local_runtime().value_or("")); });
        }
    }

    if (models::ModelRuntime* runtime = parts_.models) {
        models::ModelRuntime& r = *runtime;
        links_.push_back(r.hf_consent_requested.connect(
            ui_, [this, runtime](std::string model, bool envBlocked, bool /*loadIntoEngine*/) {
                runtime->answer_consent(model, askConsent(q(model), envBlocked));
            }));
        links_.push_back(r.runtime_consent_requested.connect(
            ui_, [this, runtime](std::string model, std::string component) {
                runtime->answer_runtime(component, askRequiredRuntime(q(model), q(component)));
            }));
        links_.push_back(r.gpu_runtime_offer_requested.connect(
            ui_, [this, runtime](std::string model, std::string gpu, std::string cpu) {
                runtime->answer_gpu_offer(model, askGpuRuntime(q(model), q(gpu), q(cpu)));
            }));
        links_.push_back(r.model_download_started.connect(
            ui_, [this](std::string model) { onModelDownloadStarted(q(model)); }));
        links_.push_back(r.model_download_progress.connect(
            ui_, [this](std::string model, std::int64_t done, std::int64_t total) {
                onModelDownloadProgress(q(model), done, total);
            }));
        links_.push_back(
            r.model_download_finished.connect(ui_, [this](std::string model, bool success) {
                onModelDownloadFinished(q(model), success);
            }));
        links_.push_back(
            r.batch_download_planned.connect(ui_, [this](std::vector<std::string> models) {
                QStringList names;
                for (const std::string& name : models) {
                    names.append(q(name));
                }
                onModelBatchPlanned(names);
            }));
        links_.push_back(r.batch_download_finished.connect(
            ui_, [this](int completed, int planned) { onModelBatchFinished(completed, planned); }));
        links_.push_back(r.model_deleted.connect(
            ui_, [this](std::string model, bool success, std::string error) {
                onModelDeleted(q(model), success, q(error));
            }));
        links_.push_back(r.model_cache_changed.connect(ui_, [this] { refreshModelViews(); }));
        links_.push_back(r.hf_settings_requested.connect(ui_, [this] {
            if (parts_.settingsWindow != nullptr) {
                parts_.settingsWindow->openHfPolicy();
            }
        }));
        links_.push_back(r.downloads_requested.connect(ui_, [this](std::string component) {
            if (parts_.settingsWindow != nullptr) {
                parts_.settingsWindow->openDownloads(q(component));
            }
        }));
    }

    if (models::ComponentRuntime* runtime = parts_.components) {
        models::ComponentRuntime& r = *runtime;
        links_.push_back(
            r.component_progress.connect(ui_, [this](std::string component, std::string phase,
                                                     std::int64_t done, std::int64_t total) {
                onComponentProgress(q(component), q(phase), done, total);
            }));
        links_.push_back(r.component_install_started.connect(
            ui_, [this](std::string /*component*/) { onComponentStateChanged(); }));
        links_.push_back(r.component_install_finished.connect(
            ui_, [this](std::string component, bool success, std::string message) {
                onComponentInstallFinished(q(component), success, q(message));
            }));
        links_.push_back(
            r.component_state_changed.connect(ui_, [this] { onComponentStateChanged(); }));
    }
}

// ---- the page ----

// The Settings window was just made: its Downloads page exists from now on, deferred, so the
// rail can read it before the page is opened.
void DownloadsPresenter::onDialogCreated(SettingsDialog* dialog) {
    if (dialog == nullptr || (page_ != nullptr && pageDialog_ == dialog)) {
        return;
    }
    // Parentless until the window builds its Downloads destination: the window's widget tree
    // holds only what it shows. The page goes with the window if it was never placed.
    auto* page = new DownloadsPage(settings_, io_, pageOptions(), true);
    connect(dialog, &QObject::destroyed, page, [page] {
        if (page->parent() == nullptr) {
            page->deleteLater();
        }
    });
    page_ = page;
    pageDialog_ = dialog;
    connectPage(page, dialog);
    page->refresh(false);
}

DownloadsPage* DownloadsPresenter::pageFor(SettingsDialog* dialog, QWidget* parent) {
    if (page_ != nullptr && !page_->isUiBuilt() && page_->parent() == nullptr &&
        dialog != nullptr && pageDialog_ == dialog) {
        DownloadsPage* page = page_;
        page->setParent(parent);
        page->ensureUi();
        page->setVisible(true);
        return page;
    }
    auto* page = new DownloadsPage(settings_, io_, pageOptions(), false, parent);
    if (page_ == nullptr || pageDialog_ == dialog) {
        page_ = page;
        pageDialog_ = dialog;
    }
    connectPage(page, dialog);
    return page;
}

void DownloadsPresenter::connectPage(DownloadsPage* page, SettingsDialog* dialog) {
    connect(page, &DownloadsPage::downloadRequested, this, [this](const QString& model) {
        if (parts_.models != nullptr) {
            parts_.models->request_download(u(model), false);
        }
    });
    connect(page, &DownloadsPage::deleteRequested, this, [this](const QString& model) {
        if (parts_.models != nullptr) {
            parts_.models->request_delete(u(model));
        }
    });
    connect(page, &DownloadsPage::batchDownloadRequested, this, [this](const QStringList& names) {
        if (parts_.models != nullptr) {
            std::vector<std::string> models;
            for (const QString& name : names) {
                models.push_back(u(name));
            }
            parts_.models->request_batch(models);
        }
    });
    connect(page, &DownloadsPage::batchCancelRequested, this, [this] {
        if (parts_.models != nullptr) {
            parts_.models->stop_batch();
        }
    });
    connect(page, &DownloadsPage::componentInstallRequested, this, [this](const QString& id) {
        if (parts_.components != nullptr) {
            parts_.components->request_install(u(id));
        }
    });
    connect(page, &DownloadsPage::componentCancelRequested, this, [this](const QString& id) {
        if (parts_.components != nullptr) {
            parts_.components->cancel_install(u(id));
        }
    });
    connect(page, &DownloadsPage::componentRemoveRequested, this, [this](const QString& id) {
        if (parts_.components != nullptr) {
            parts_.components->request_uninstall(u(id));
        }
    });
    if (dialog == nullptr) {
        return;
    }
    QPointer<SettingsDialog> window(dialog);
    connect(page, &DownloadsPage::inventoryChanged, dialog, [window] {
        if (window != nullptr) {
            window->refreshRailValues();
        }
    });
    // Once per percent: nothing but the Downloads caption changes.
    connect(page, &DownloadsPage::progressChanged, dialog, [window, page] {
        if (window != nullptr) {
            window->rail()->setValue(downloadsKey(), page->railValue());
        }
    });
    // Showing the window rescans (refresh_models in its showEvent).
    connect(dialog, &SettingsDialog::windowShown, page, [page] { page->refresh(true); });
}

SettingsDialog* DownloadsPresenter::settingsDialog() const {
    if (parts_.settingsWindow != nullptr) {
        return parts_.settingsWindow->dialog();
    }
    return pageDialog_;
}

bool DownloadsPresenter::settingsVisible() const {
    const SettingsDialog* dialog = settingsDialog();
    return dialog != nullptr && dialog->isVisible();
}

// ---- events ----

void DownloadsPresenter::setTabsDownloading(const QString& modelName, bool downloading) {
    if (parts_.record == nullptr) {
        return;
    }
    if (QuickRecordTab* tab = parts_.record->existingQuickRecord()) {
        tab->setModelDownloading(modelName, downloading);
    }
    if (UploadFileTab* tab = parts_.record->existingUpload()) {
        tab->setModelDownloading(modelName, downloading);
    }
}

// The Voice model page's set_downloading, finish_download and refresh_component_state: it
// redraws what the engine has on this computer.
void DownloadsPresenter::refreshVoicePage() {
    SettingsDialog* dialog = settingsDialog();
    if (dialog == nullptr) {
        return;
    }
    if (auto* voice =
            qobject_cast<VoiceModelPage*>(dialog->pageContent(destinations::voice_model))) {
        voice->refresh(true);
    }
}

void DownloadsPresenter::onModelDownloadStarted(const QString& modelName) {
    setTabsDownloading(modelName, true);
    if (page_ != nullptr) {
        page_->setDownloading(modelName);
        refreshVoicePage();
    }
    Q_EMIT modelDownloadStarted(modelName);
    // Settings shows its own progress (the row and the rail), and so does the guided setup for
    // a download started there.
    if (downloadShownElsewhere_ && downloadShownElsewhere_(modelName)) {
        return;
    }
    if (!settingsVisible()) {
        showDownloadProgress(modelName);
    }
}

void DownloadsPresenter::onModelDownloadProgress(const QString& modelName, std::int64_t done,
                                                 std::int64_t total) {
    if (page_ != nullptr) {
        page_->setDownloadProgress(modelName, done, total);
    }
    Q_EMIT modelDownloadProgress(modelName, done, total);
    updateDownloadProgress(modelName, done, total);
}

void DownloadsPresenter::onModelDownloadFinished(const QString& modelName, bool success) {
    setTabsDownloading(modelName, false);
    if (page_ != nullptr) {
        page_->finishDownload(modelName, success);
        refreshVoicePage();
    }
    Q_EMIT modelDownloadFinished(modelName, success);
    hideDownloadProgress();
    refreshModelViews();
}

void DownloadsPresenter::onModelBatchPlanned(const QStringList& modelNames) {
    if (page_ != nullptr) {
        page_->beginBatch(modelNames);
    }
}

void DownloadsPresenter::onModelBatchFinished(int completed, int planned) {
    if (page_ != nullptr) {
        page_->finishBatch(completed, planned);
    }
}

void DownloadsPresenter::onModelDeleted(const QString& modelName, bool success,
                                        const QString& error) {
    if (page_ != nullptr) {
        page_->showDeleteResult(modelName, success, error);
    }
    refreshModelViews();
}

void DownloadsPresenter::onComponentProgress(const QString& componentId, const QString& phase,
                                             std::int64_t done, std::int64_t total) {
    if (page_ != nullptr) {
        page_->setComponentProgress(componentId, phase, done, total);
    }
    Q_EMIT componentProgress(componentId, done, total);
}

void DownloadsPresenter::onComponentStateChanged() {
    if (page_ == nullptr) {
        return;
    }
    page_->refreshComponents();
    SettingsDialog* dialog = settingsDialog();
    if (dialog == nullptr) {
        return;
    }
    if (auto* voice =
            qobject_cast<VoiceModelPage*>(dialog->pageContent(destinations::voice_model))) {
        // Only what the engine has on this computer depends on the runtimes.
        voice->refresh(false);
    }
}

void DownloadsPresenter::onComponentInstallFinished(const QString& componentId, bool success,
                                                    const QString& message) {
    if (page_ != nullptr) {
        page_->finishComponentInstall(componentId, success, message);
        if (SettingsDialog* dialog = settingsDialog()) {
            if (auto* voice =
                    qobject_cast<VoiceModelPage*>(dialog->pageContent(destinations::voice_model))) {
                voice->refresh(false);
            }
        }
    }
    Q_EMIT componentInstallFinished(componentId, success, message);
}

void DownloadsPresenter::refreshModelViews() {
    if (!settingsVisible()) {
        return;
    }
    refreshVoicePage();
    if (page_ != nullptr) {
        page_->refresh(true);
    }
}

// ---- the fallback progress dialog ----

void DownloadsPresenter::showDownloadProgress(const QString& modelName) {
    hideDownloadProgress();
    auto* dialog = new QProgressDialog(QStringLiteral("Downloading \"%1\"…").arg(modelName),
                                       QString(), 0, 0, parentWidget());
    dialog->setWindowTitle(QStringLiteral("Downloading model"));
    dialog->setMinimumDuration(0);
    dialog->setCancelButton(nullptr);
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    dialog->setMinimumWidth(360);
    dialog->show();
    progressDialog_ = dialog;
}

void DownloadsPresenter::updateDownloadProgress(const QString& modelName, std::int64_t done,
                                                std::int64_t total) {
    QProgressDialog* dialog = progressDialog_;
    if (dialog == nullptr) {
        return;
    }
    if (total <= 0) {
        dialog->setRange(0, 0);
        dialog->setLabelText(QStringLiteral("Downloading \"%1\"…").arg(modelName));
        return;
    }
    dialog->setRange(0, 100);
    dialog->setValue(static_cast<int>(done * 100 / total));
    dialog->setLabelText(
        QStringLiteral("Downloading \"%1\"… %2 of %3")
            .arg(modelName, q(format_size_bytes(done)), q(format_size_bytes(total))));
}

void DownloadsPresenter::hideDownloadProgress() {
    QProgressDialog* dialog = progressDialog_;
    progressDialog_ = nullptr;
    if (dialog != nullptr) {
        dialog->close();
        dialog->deleteLater();
    }
}

// ---- the questions ----

QWidget* DownloadsPresenter::parentWidget() const {
    return dialogParent ? dialogParent() : nullptr;
}

int DownloadsPresenter::run(QDialog& dialog) {
    return runDialog ? runDialog(dialog) : dialog.exec();
}

models::ConsentAction DownloadsPresenter::askConsent(const QString& modelName, bool envBlocked) {
    const std::string policy = models::load_hf_access_policy(settings_);
    const models::Host computer = host ? host() : models::Host{};
    const models::RuntimeNeeds needs =
        models::runtime_needs(u(modelName), *settings_.snapshot(), computer);
    HuggingFaceConsentDialog dialog(u(modelName), policy, envBlocked, needs, parentWidget());
    run(dialog);
    return dialog.resultAction();
}

bool DownloadsPresenter::askRequiredRuntime(const QString& modelName, const QString& componentId) {
    const models::Host computer = host ? host() : models::Host{};
    RequiredRuntimeDialog dialog(u(modelName), u(componentId), computer, parentWidget());
    return run(dialog) == QDialog::Accepted;
}

std::optional<std::string> DownloadsPresenter::askGpuRuntime(const QString& modelName,
                                                             const QString& gpuComponent,
                                                             const QString& cpuComponent) {
    const models::Host computer = host ? host() : models::Host{};
    GpuRuntimeDialog dialog(u(modelName), u(gpuComponent), u(cpuComponent), computer.nvidia(),
                            computer, parentWidget());
    run(dialog);
    return dialog.choice();
}

} // namespace fv
