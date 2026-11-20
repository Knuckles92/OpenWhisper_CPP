#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>
#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"
#include "models/catalog.h"
#include "models/downloads_model.h"
#include "models/hf_access.h"
#include "models/model_details.h"
#include "ui_settings/settings_page.h"
#include "ui_settings/settings_search_source.h"

class QDialog;
class QFrame;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMessageBox;
class QPushButton;
class QScrollArea;
class QUrl;
class QVBoxLayout;

namespace fv {

class Button;
class ButtonRow;
class ComponentRowWidget;
class ElidingComboBox;
class ElidingLabel;
class EmptyState;
class EnvReader;
class Executor;
class ModelRowWidget;
class SegmentedBar;
class SettingsStore;
class WrappedLabel;

// What the Downloads page reads of this computer, so a test answers it without a disk, a driver
// or a network. Every member is optional.
struct DownloadsPageOptions {
    // The last shared inventory of downloaded models without touching the disk
    // (CachedModelsSnapshot::peek); nullopt before the first scan. Unset: none yet.
    std::function<std::optional<models::CachedModelsSnapshot::Inventory>()> peekInventory;
    // A fresh inventory, at most 30 s old (CachedModelsSnapshot::scan(30)). Blocks on the disk:
    // it runs on the io executor, or on the GUI thread when backgroundScan is false. What it
    // throws is logged and reads as nothing downloaded. Unset: nothing is downloaded.
    std::function<models::CachedModelsSnapshot::Inventory()> scanInventory;
    // The model the engine has loaded (EngineControl::loaded_model_key through ModelRuntime): its
    // files are mapped, so Delete stays disabled for it. Never a call into the engine on the GUI
    // thread. Unset: none is loaded.
    std::function<std::optional<std::string>()> loadedModel;
    // The local speech model meetings use (resolve_meeting_whisper_model, "" when meetings use a
    // paired computer's speech): models::meeting_model_in_use in the app. Unset, no model is a
    // meeting's.
    std::function<std::string(const Json& settings)> meetingModel;
    // The optional components installable here (ComponentCoordinator::list_components), their
    // current description and whether an install is in flight. Unset: none.
    std::function<std::vector<models::ComponentInfo>()> listComponents;
    std::function<models::ComponentInfo(const std::string& componentId)> describeComponent;
    std::function<bool(const std::string& componentId)> isInstalling;
    // HF_HUB_OFFLINE; nullptr: system_env().
    const EnvReader* env = nullptr;
    // The folder the speech models are stored in (Paths::speech_models_root): the header names
    // it, Open folder opens it and the batch dialog shows it.
    QString modelsFolder;
    // Python's background_cache_scan: false scans on the GUI thread (tests).
    bool backgroundScan = true;
};

// Settings > Models & storage > Downloads (ui_qt/dialogs/settings_downloads.py DownloadsPage):
// the speech-model catalog with its filters, sorts, batch selection and the side profile
// (inspector) of the selected model, and the optional components on a second tab. Settings is
// non-modal, so a multi-gigabyte download never locks the user out of recording; only the
// catalog, the inspector's profile and the components tab scroll.
//
// The page is built lazily (Python's defer_build and ensure_ui): until ensureUi() only the
// inventory, its sizes, the download in flight and the message are kept, so a hidden Settings
// never builds the catalog. Every widget accessor builds it, as Python's __getattr__ does.
//
// The inspector docks beside the catalog when the page is at least the catalog's minimum width
// plus 12 plus 300 px wide; otherwise it floats over the catalog's right edge with a drop shadow
// and a close button, shown only once a row is picked. Re-evaluated on show and resize.
//
// Scans run on the io executor and come back by generation through a mailbox the page clears
// when it goes, so a worker never holds the page and a stale scan is dropped. Downloads,
// deletes and installs are asked for through the signals; the presenter reports back through the
// setters. Custom faster-whisper models (OpenWhisper's "Add custom models…" and their rows) are
// not ported.
// Affinity: GUI thread.
class DownloadsPage : public QWidget,
                      public SettingsPageModule,
                      public SettingsDeepLinks,
                      public SettingsSearchSource {
    Q_OBJECT
public:
    // Width the profile takes when it docks beside the catalog.
    static constexpr int kInspectorWidth = 300;

    // The settings store outlives the page; `io` runs the scans and must outlive every scan the
    // page started. With deferBuild the widgets wait for ensureUi().
    DownloadsPage(SettingsStore& settings, Executor& io, DownloadsPageOptions options = {},
                  bool deferBuild = false, QWidget* parent = nullptr);
    ~DownloadsPage() override;

    // Builds the widgets once and shows the inventory known so far.
    void ensureUi();
    bool isUiBuilt() const { return uiBuilt_; }

    // add_policy_control: a labelled control on the row under the header (the Hugging Face
    // policy of Settings).
    void addPolicyControl(const QString& label, QWidget* control);

    // --- Deep links and the inspector ---
    // Shows one catalog model's bundled profile in the inspector; an unknown name is ignored.
    void selectModel(const QString& modelName);
    // Clears the filters that hide a model, shows the models tab, selects it and scrolls to it.
    void revealModel(const QString& modelName) override;
    // Shows the models tab filtered to one backend; "" or an unknown backend shows every model.
    void showBackend(const QString& backend) override;
    // Shows the components tab with one component selected, scrolled to and focused.
    void focusComponent(const QString& componentId) override;
    // Hides the floating profile; a docked profile always stays open.
    void closeInspector();
    bool inspectorDocked() const { return inspectorDocked_; }

    // --- State, driven by the presenter ---
    // Re-reads the inventory: the last shared scan at once and, with scan, a fresh one on the io
    // executor. Without scan the disk is never touched (a hidden window loading).
    void refresh(bool scan = true);
    void refreshComponents();
    void setComponentProgress(const QString& componentId, const QString& phase, std::int64_t done,
                              std::int64_t total);
    void finishComponentInstall(const QString& componentId, bool success, const QString& message);
    void setDownloading(const QString& modelName);
    void setDownloadProgress(const QString& modelName, std::int64_t done, std::int64_t total);
    void finishDownload(const QString& modelName, bool success);
    // Enters batch mode with the approved plan (only names with rows are kept).
    void beginBatch(const QStringList& modelNames);
    // Closes the batch with a summary of what actually downloaded.
    void finishBatch(int completed, int planned);
    void showDeleteResult(const QString& modelName, bool success, const QString& error);

    // --- Summaries read by Settings (rail, Home) ---
    // The first scan is still running.
    bool isChecking() const { return inventoryLoading_; }
    models::StorageSummary storageSummary() const;
    // The download in flight, else "Checking downloaded models…", else the cache totals.
    QString railValue() const;
    // The shared message line's text.
    QString messageText() const { return message_; }

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    void refreshPage() override;
    QWidget* namedControl(std::string_view name) override;

    // SettingsSearchSource (_catalog_entries for a built page): each model row as "<label>",
    // "Downloads · <size> · <summary>", badged with its usage chip when shown and "Downloaded"
    // when cached; each component row as its name, "Downloads › Components · <summary>", badged
    // "Installed" when usable. nullopt until the widgets are built.
    std::optional<std::vector<appcore::SearchEntry>> searchEntries() override;

    // Where the Settings window's policy combo is registered: its load runs with the page's.
    void setPolicyLoader(std::function<void(const Json&)> loader) {
        policyLoader_ = std::move(loader);
    }
    void setPolicyControl(QWidget* control) { policyControl_ = control; }

    // Seams a test replaces: QMessageBox::exec for the delete and remove questions, QDialog::exec
    // for the batch confirmation and the component profile, and the link opener
    // (QDesktopServices::openUrl).
    std::function<int(QMessageBox&)> runMessageBox;
    std::function<int(QDialog&)> runDialog;
    std::function<void(const QUrl&)> openUrl;

    // --- Widgets (each builds the page first) ---
    SegmentedBar* tabs();
    QWidget* modelsSection();
    QScrollArea* componentsSection();
    QScrollArea* componentsScroll() { return componentsSection(); }
    ElidingLabel* statsLabel();
    Button* downloadAllButton();
    Button* openFolderButton();
    QHBoxLayout* policyRow();
    QLabel* envBanner();
    QWidget* catalogColumn();
    QLineEdit* filterEdit();
    ElidingComboBox* backendFilterCombo();
    ElidingComboBox* statusFilterCombo();
    ElidingComboBox* sortCombo();
    QHBoxLayout* toolbarLayout();
    Button* selectAllButton();
    Button* clearSelectionButton();
    ElidingLabel* selectionSummary();
    Button* downloadSelectedButton();
    QScrollArea* libraryScrollArea();
    QVBoxLayout* listLayout();
    QLabel* emptyLabel();
    QLabel* messageLabel();
    Button* stopBatchButton();
    // The rows in catalog order.
    const std::vector<std::pair<QString, ModelRowWidget*>>& rows();
    ModelRowWidget* row(const QString& modelName);
    const std::vector<std::pair<QString, ComponentRowWidget*>>& componentRows();
    ComponentRowWidget* componentRow(const QString& componentId);
    // The rows in the order the list shows them.
    QStringList rowOrder();

    QFrame* inspector();
    QPushButton* inspectorCloseButton();
    ElidingLabel* inspectorName();
    EmptyState* inspectorEmpty();
    ElidingLabel* inspectorTags();
    QScrollArea* inspectorDetailScroll();
    WrappedLabel* inspectorDescription();
    QLabel* inspectorBestForHeading();
    WrappedLabel* inspectorBestFor();
    QLabel* inspectorTradeoffsHeading();
    WrappedLabel* inspectorTradeoffs();
    WrappedLabel* inspectorSourceNote();
    QLabel* inspectorUsage();
    Button* inspectorRepoButton();
    Button* inspectorOriginButton();
    Button* inspectorLicenseButton();
    ButtonRow* inspectorLinks();
    // The value label of a profile row by its caption ("Repository", "Local format", ...).
    WrappedLabel* factLabel(const QString& caption);
    std::optional<QString> selectedModel() const { return selectedModel_; }

    // The selection bar's handlers (Select all, Clear), callable as Python's tests do.
    void selectAllMissing();
    void clearSelection();
    // Asks before removing a component (_confirm_component_removal).
    void confirmComponentRemoval(const QString& componentId);

Q_SIGNALS:
    void componentInstallRequested(const QString& componentId);
    void componentCancelRequested(const QString& componentId);
    void componentRemoveRequested(const QString& componentId);
    void downloadRequested(const QString& modelName);
    void deleteRequested(const QString& modelName);
    // The confirmed batch, in catalog order.
    void batchDownloadRequested(const QStringList& modelNames);
    void batchCancelRequested();
    // Cache totals or download state changed (the rail value, Home).
    void inventoryChanged();
    // Only railValue changed: a running download's rounded percentage.
    void progressChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Mailbox;
    struct RowContext;

    void setupUi();
    QWidget* buildCatalogColumn();
    QWidget* buildSelectionBar();
    QWidget* buildList();
    QFrame* buildInspector();
    QScrollArea* buildComponentsSection();
    QFrame* buildComponentStrip();
    void showSection(int index);

    void syncToolbarGutter();
    int catalogMinWidth() const;
    void updateInspectorMode();
    void renderInspector();
    void setInspectorEnabled(bool enabled);
    void openDetailsUrl(const QString& url);
    void openCacheFolder();

    void onDownloadClicked(const QString& modelName);
    void onDeleteClicked(const QString& modelName);
    void onSelectionToggled(const QString& modelName, bool checked);
    std::vector<std::string> selectedNames() const;
    std::vector<std::string> missingNames() const;
    void updateSelectionBar();
    void requestBatch(const std::vector<std::string>& modelNames);
    void showComponentDetails(const QString& componentId);
    int askQuestion(const QString& title, const QString& text);

    void onScanFinished(models::CachedModelsSnapshot::Inventory inventory);
    void applyInventory(const models::CachedModelsSnapshot::Inventory& inventory);
    void refreshRow(ModelRowWidget* row, const std::optional<models::CachedModelInfo>& info,
                    const RowContext& context);
    void showCacheTotals();
    void applyFilter();
    Json settingsSnapshot() const;
    models::ModelsInUse modelsInUse(const Json& settings) const;
    void setMessage(const QString& text);
    const EnvReader& env() const;

    SettingsStore& settings_;
    Executor& io_;
    DownloadsPageOptions options_;
    std::shared_ptr<Mailbox> mailbox_;
    std::function<void(const Json&)> policyLoader_;
    QWidget* policyControl_ = nullptr;

    bool uiBuilt_ = false;
    bool inventoryLoading_ = false;
    models::CachedModelsSnapshot::Inventory inventory_;
    models::DownloadQueue queue_;
    QString message_;
    std::optional<QString> selectedModel_;
    std::set<std::string> selectedModels_;
    bool downloadsBlocked_ = false;
    bool inspectorDocked_ = true;
    bool inspectorOpen_ = false;
    const models::ModelDetails* details_ = nullptr;

    SegmentedBar* tabs_ = nullptr;
    QWidget* modelsSection_ = nullptr;
    QScrollArea* componentsSection_ = nullptr;
    ElidingLabel* statsLabel_ = nullptr;
    Button* downloadAllButton_ = nullptr;
    Button* openFolderButton_ = nullptr;
    QHBoxLayout* policyRow_ = nullptr;
    QLabel* envBanner_ = nullptr;
    QHBoxLayout* split_ = nullptr;
    QWidget* catalogColumn_ = nullptr;
    QHBoxLayout* toolbarLayout_ = nullptr;
    QLineEdit* filterEdit_ = nullptr;
    ElidingComboBox* backendFilterCombo_ = nullptr;
    ElidingComboBox* statusFilterCombo_ = nullptr;
    ElidingComboBox* sortCombo_ = nullptr;
    Button* selectAllButton_ = nullptr;
    Button* clearSelectionButton_ = nullptr;
    ElidingLabel* selectionSummary_ = nullptr;
    Button* downloadSelectedButton_ = nullptr;
    QScrollArea* libraryScrollArea_ = nullptr;
    QVBoxLayout* listLayout_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QLabel* messageLabel_ = nullptr;
    Button* stopBatchButton_ = nullptr;
    std::vector<std::pair<QString, ModelRowWidget*>> rows_;
    std::vector<std::pair<QString, ComponentRowWidget*>> componentRows_;

    QFrame* inspector_ = nullptr;
    QPushButton* inspectorCloseButton_ = nullptr;
    ElidingLabel* inspectorName_ = nullptr;
    EmptyState* inspectorEmpty_ = nullptr;
    ElidingLabel* inspectorTags_ = nullptr;
    QScrollArea* inspectorDetailScroll_ = nullptr;
    WrappedLabel* inspectorDescription_ = nullptr;
    QGridLayout* inspectorFacts_ = nullptr;
    QLabel* inspectorBestForHeading_ = nullptr;
    WrappedLabel* inspectorBestFor_ = nullptr;
    QLabel* inspectorTradeoffsHeading_ = nullptr;
    WrappedLabel* inspectorTradeoffs_ = nullptr;
    WrappedLabel* inspectorSourceNote_ = nullptr;
    QLabel* inspectorUsage_ = nullptr;
    Button* inspectorRepoButton_ = nullptr;
    Button* inspectorOriginButton_ = nullptr;
    Button* inspectorLicenseButton_ = nullptr;
    ButtonRow* inspectorLinks_ = nullptr;
    std::map<QString, WrappedLabel*> factLabels_;
};

} // namespace fv
