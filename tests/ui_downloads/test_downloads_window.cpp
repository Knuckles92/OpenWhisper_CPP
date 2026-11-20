// The Downloads page inside the Settings window, and DownloadsPresenter between the page, the
// model and component runtimes and the other views (ui_controller.py's download hooks): the
// Settings routing tests of test_hf_consent_dialog.py and test_settings_unified.py, and the
// presenter's routing of the runtimes' questions, events and the fallback progress dialog. The
// runtimes run over fakes: an in-memory settings file, a fake engine, the downloads and
// components strands as manual executors (never run, so nothing downloads or installs).
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>

#include "appkit/destinations.h"
#include "appkit/registries.h"
#include "base/json.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "downloads_page_support.h"
#include "history/format_utils.h"
#include "http/testing/fake_http_client.h"
#include "models/component_details.h"
#include "models/component_runtime.h"
#include "models/component_store.h"
#include "models/coordinator.h"
#include "models/hf_access.h"
#include "models/hf_access_policy.h"
#include "models/model_runtime.h"
#include "models/speech_model_cache.h"
#include "models/testing/fake_engine_control.h"
#include "models/testing/fake_openwhisper.h"
#include "ui_downloads/component_row_widget.h"
#include "ui_downloads/downloads_feature.h"
#include "ui_downloads/downloads_page.h"
#include "ui_downloads/downloads_presenter.h"
#include "ui_downloads/hf_consent_dialog.h"
#include "ui_downloads/model_row_widget.h"
#include "ui_downloads/required_runtime_dialog.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "voice_model_support.h"

using fv::DownloadsPage;
using fv::DownloadsPresenter;
using fv::Json;
using fv::SettingsDialog;
using fv::SettingsWindow;
using fvtest::kOrukeet;
using fvtest::kParakeet;
using fvtest::kParakeetBytes;
using fvtest::kParakeetRepo;

namespace fs = std::filesystem;

namespace {

QString q(const std::string& text) {
    return QString::fromStdString(text);
}

QString downloadsKey() {
    return QString::fromUtf8(fv::destinations::downloads);
}

// A fresh folder under the system temp folder, removed with everything in it at the end.
class TempFolder {
public:
    TempFolder() {
        static std::atomic<int> counter{0};
        std::random_device seed;
        path_ = fs::temp_directory_path() /
                ("fv-downloads-test-" + std::to_string(seed()) + "-" + std::to_string(++counter));
        fs::create_directories(path_);
    }
    ~TempFolder() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// The Settings window over a context with the Downloads page registered through a presenter
// whose pages read the fakes of `pages`.
struct WindowHarness {
    explicit WindowHarness(const Json& saved = Json{{"selected_model", "parakeet"}})
        : window(saved), presenter(window.settings(), window.ui(), window.io()) {
        presenter.setPageOptions([this] { return pages.options(); });
        // Home, where the window opens, so Downloads is built only when it is opened.
        fv::SettingsPageSpec home;
        home.destination = fv::destinations::overview;
        home.title = QStringLiteral("Home");
        home.create = [](QWidget* parent) {
            return parent;
        };
        window.context().settingsPages().addPage(std::move(home));
        fv::registerDownloadsPage(window.context(), presenter);
        settingsWindow = std::make_unique<SettingsWindow>(window.context());
        settingsWindow->raiseDialog = [](SettingsDialog&) {
        };
    }

    void attach(fv::DownloadsPresenterParts parts = {}) {
        parts.settingsWindow = settingsWindow.get();
        presenter.attach(parts);
    }

    DownloadsPage* page() const { return presenter.page(); }

    fvtest::WindowRig window;
    fvtest::DownloadsRig pages;
    DownloadsPresenter presenter;
    std::unique_ptr<SettingsWindow> settingsWindow;
};

// The model and component runtimes over fakes, on a Windows computer with `card` as its GPU.
struct RuntimeHarness {
    explicit RuntimeHarness(fv::SettingsStore& store,
                            std::optional<fv::models::NvidiaGpu> card = std::nullopt)
        : settings(store) {
        fv::models::ComponentStoreOptions storeOptions;
        storeOptions.host = fvtest::windowsHost();
        if (card) {
            storeOptions.host->gpu = [card] {
                return card;
            };
        }
        storeOptions.free_bytes = [](const fs::path&) {
            return std::uint64_t{1} << 40;
        };
        componentStore = std::make_unique<fv::models::ComponentStore>(temp.path() / "components",
                                                                      client, storeOptions);
        fv::models::ComponentCoordinator::Options coordinatorOptions;
        coordinatorOptions.copy_for = &fv::models::component_copy;
        coordinator =
            std::make_unique<fv::models::ComponentCoordinator>(*componentStore, coordinatorOptions);
        fv::models::SpeechModelCacheOptions cacheOptions;
        cacheOptions.env = &env;
        cache = std::make_unique<fv::models::SpeechModelCache>(temp.path() / "speech-models",
                                                               client, cacheOptions);
        access = std::make_unique<fv::models::HuggingFaceAccessCoordinator>(
            settings, [this](std::string_view name) { return cache->is_cached(name); }, env);
        fv::models::ModelRuntimeOptions options;
        options.env = &env;
        models = std::make_unique<fv::models::ModelRuntime>(engine, *cache, *access, *coordinator,
                                                            settings, ui, work, clock, options);
        components =
            std::make_unique<fv::models::ComponentRuntime>(engine, *coordinator, ui, strand, clock);
        models->set_component_runtime(components.get());
    }

    fv::models::Host host() const { return componentStore->host(); }

    fv::SettingsStore& settings;
    TempFolder temp;
    fv::http::FakeHttpClient client;
    fv::MapEnvReader env;
    fv::FakeClock clock;
    fv::ManualExecutor ui;
    fv::ManualExecutor work;
    fv::ManualExecutor strand;
    fv::models::FakeEngineControl engine;
    std::unique_ptr<fv::models::ComponentStore> componentStore;
    std::unique_ptr<fv::models::ComponentCoordinator> coordinator;
    std::unique_ptr<fv::models::SpeechModelCache> cache;
    std::unique_ptr<fv::models::HuggingFaceAccessCoordinator> access;
    std::unique_ptr<fv::models::ModelRuntime> models;
    std::unique_ptr<fv::models::ComponentRuntime> components;
};

fv::models::NvidiaGpu rtx2060() {
    fv::models::NvidiaGpu gpu;
    gpu.name = "NVIDIA GeForce RTX 2060";
    gpu.total_mib = 6144;
    gpu.compute_capability = std::pair{7, 5};
    return gpu;
}

} // namespace

// ---- The page in the Settings window ----

// from
// tests/test_hf_consent_dialog.py::TestSettingsDialogNavigation::test_focus_hf_policy_selects_downloads_destination
TEST_CASE("focus hf policy selects downloads destination", "[test_hf_consent_dialog][widget]") {
    WindowHarness harness;
    SettingsDialog dialog(harness.window.context());
    dialog.focusHfPolicy();

    CHECK(dialog.currentDestination() == downloadsKey());
    auto* page = qobject_cast<DownloadsPage*>(dialog.pageContent(fv::destinations::downloads));
    REQUIRE(page != nullptr);
    auto* combo = qobject_cast<QComboBox*>(dialog.control("hf_policy_combo"));
    REQUIRE(combo != nullptr);
    CHECK(page->isAncestorOf(combo));
    CHECK(combo->objectName() == "hfPolicyCombo");
    std::set<std::string> policies;
    for (int i = 0; i < combo->count(); ++i) {
        policies.insert(combo->itemData(i).toString().toStdString());
    }
    CHECK(policies == std::set<std::string>{"ask", "always", "never"});
}

// from tests/test_settings_unified.py::TestRouting::test_hf_policy_persists_from_the_downloads_page
TEST_CASE("hf policy persists from the downloads page", "[test_settings_unified][widget]") {
    WindowHarness harness(Json{{"selected_model", "parakeet"}, {"hf_hub_offline", true}});
    SettingsDialog dialog(harness.window.context());
    auto* combo = qobject_cast<QComboBox*>(dialog.control("hf_policy_combo"));
    REQUIRE(combo != nullptr);
    // Loading migrated the legacy value without being saved again by the combo.
    CHECK(combo->currentData().toString() == "never");
    fvtest::pick(combo, "always");
    CHECK(fv::models::load_hf_access_policy(harness.window.settings()) == "always");
    combo->setCurrentIndex(combo->findData(QStringLiteral("never")));
    CHECK(fv::models::load_hf_access_policy(harness.window.settings()) == "never");
    CHECK_FALSE(harness.window.settings().snapshot()->contains("hf_hub_offline"));
}

// A11-55, A11-89: the policy field as settings_dialog.py _build_downloads_page builds it.
TEST_CASE("the policy field is captioned, sized and explained as OpenWhisper's", "[widget]") {
    WindowHarness harness;
    SettingsDialog dialog(harness.window.context());
    auto* combo = qobject_cast<QComboBox*>(dialog.control("hf_policy_combo"));
    REQUIRE(combo != nullptr);
    QStringList items;
    for (int i = 0; i < combo->count(); ++i) {
        items.append(combo->itemText(i));
    }
    CHECK(items == QStringList{QStringLiteral("Ask before downloading"),
                               QStringLiteral("Always allow downloads"),
                               QStringLiteral("Never connect (fully offline)")});
    CHECK(combo->maximumWidth() == 300);
    CHECK(combo->minimumHeight() == 34);
    CHECK(combo->toolTip().startsWith(
        QStringLiteral("Models already on this computer always load locally")));
    CHECK(combo->toolTip().endsWith(QStringLiteral(
        "An external HF_HUB_OFFLINE=1 environment variable disables downloads entirely.")));
    QWidget* page = dialog.pageContent(fv::destinations::downloads);
    REQUIRE(page != nullptr);
    bool captioned = false;
    for (const QLabel* label : page->findChildren<QLabel*>()) {
        captioned = captioned ||
                    label->text() == QStringLiteral("When a model is missing from this computer");
    }
    CHECK(captioned);
}

// from
// tests/test_settings_unified.py::TestRouting::test_engine_downloads_focuses_the_missing_runtime
TEST_CASE("engine downloads focuses the missing runtime", "[test_settings_unified][widget]") {
    WindowHarness harness;
    harness.attach();
    harness.settingsWindow->setMissingRuntimeProvider([] { return q(fvtest::kCudaRuntime); });

    harness.settingsWindow->open(QStringLiteral("engine_downloads"));

    SettingsDialog* dialog = harness.settingsWindow->dialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->currentDestination() == downloadsKey());
    DownloadsPage* page = harness.page();
    REQUIRE(page != nullptr);
    CHECK(dialog->pageContent(fv::destinations::downloads) == page);
    CHECK(page->tabs()->currentIndex() == 1);
    CHECK(page->componentRow(q(fvtest::kCudaRuntime))->property(fv::ui::prop::selected).toBool());
    CHECK_FALSE(
        page->componentRow(q(fvtest::kCpuRuntime))->property(fv::ui::prop::selected).toBool());
}

// from
// tests/test_settings_unified.py::TestRailValues::test_downloads_rail_reports_totals_then_live_progress
TEST_CASE("downloads rail reports totals then live progress", "[test_settings_unified][widget]") {
    WindowHarness harness;
    harness.pages.cache(kParakeetRepo, kParakeetBytes);
    harness.attach();
    SettingsDialog& dialog = harness.settingsWindow->ensureDialog();
    DownloadsPage* page = harness.page();
    REQUIRE(page != nullptr);
    // The page exists, deferred, before Downloads is ever opened.
    CHECK_FALSE(page->isUiBuilt());
    page->refresh();
    CHECK(dialog.rail()->value(downloadsKey()).startsWith(QStringLiteral("1 of ")));

    page->setDownloading(q(kOrukeet));
    page->setDownloadProgress(q(kOrukeet), 50, 100);
    CHECK(dialog.rail()->value(downloadsKey()) == "Downloading Orukeet TDT 0.6B · 50%");

    page->finishDownload(q(kOrukeet), true);
    CHECK(dialog.rail()->value(downloadsKey()).startsWith(QStringLiteral("1 of ")));
    CHECK_FALSE(page->isUiBuilt());
}

TEST_CASE("the window builds the deferred page into its Downloads destination", "[widget]") {
    WindowHarness harness;
    harness.attach();
    SettingsDialog& dialog = harness.settingsWindow->ensureDialog();
    QPointer<DownloadsPage> deferred = harness.page();
    REQUIRE(deferred != nullptr);
    deferred->setDownloading(q(kOrukeet));

    dialog.selectDestination(downloadsKey());

    CHECK(dialog.pageContent(fv::destinations::downloads) == deferred);
    CHECK(deferred->isUiBuilt());
    CHECK(dialog.page(fv::destinations::downloads)->isAncestorOf(deferred));
    CHECK(dialog.pageScroll(fv::destinations::downloads) == nullptr);
    CHECK(dialog.pageTitle(fv::destinations::downloads) == "Downloads");
    CHECK(dialog.pageSubtitle(fv::destinations::downloads) ==
          "Speech models and optional components, downloaded on demand.");
    // What it kept while deferred is shown now.
    CHECK(deferred->messageLabel()->text() == "Downloading \"orukeet-v0.1\"…");
    CHECK(deferred->row(q(kOrukeet))->badge()->text() == "Downloading…");
    auto* combo = qobject_cast<QComboBox*>(dialog.control("hf_policy_combo"));
    REQUIRE(combo != nullptr);
    CHECK(deferred->isAncestorOf(combo));
    CHECK(combo->toolTip().startsWith("Models already on this computer always load locally"));
}

// ---- DownloadsPresenter ----

TEST_CASE("the presenter routes consent and the required runtime back to ModelRuntime",
          "[widget]") {
    WindowHarness harness(Json{{"selected_model", "parakeet"}, {"hf_access_policy", "ask"}});
    RuntimeHarness runtimes(harness.window.settings());
    DownloadsPresenter presenter(harness.window.settings(), runtimes.ui, harness.window.io());
    presenter.host = [&runtimes] {
        return runtimes.host();
    };
    std::vector<std::string> shown;
    presenter.runDialog = [&shown](QDialog& dialog) {
        shown.emplace_back(dialog.metaObject()->className());
        if (auto* consent = qobject_cast<fv::HuggingFaceConsentDialog*>(&dialog)) {
            CHECK(consent->titleLabel()->text().contains(q(kParakeet)));
            CHECK(consent->bodyText().contains("NVIDIA Speech CPU"));
            fvtest::buttonText(*consent, QStringLiteral("Download once"))->click();
            return consent->result();
        }
        if (auto* runtime = qobject_cast<fv::RequiredRuntimeDialog*>(&dialog)) {
            CHECK(runtime->titleLabel()->text() == "NVIDIA Speech CPU is required");
            return static_cast<int>(QDialog::Accepted);
        }
        return static_cast<int>(QDialog::Rejected);
    };
    presenter.attach({runtimes.models.get(), runtimes.components.get(), nullptr, nullptr});

    runtimes.models->request_download(kParakeet, false);
    runtimes.ui.run_all();

    CHECK(shown ==
          std::vector<std::string>{"fv::HuggingFaceConsentDialog", "fv::RequiredRuntimeDialog"});
    // Installing the runtime it needs started; then the download, which shows its progress
    // here since no Settings window shows it.
    CHECK(runtimes.coordinator->is_installing(fvtest::kCpuRuntime));
    REQUIRE(presenter.progressDialog() != nullptr);
    CHECK(presenter.progressDialog()->windowTitle() == "Downloading model");
    CHECK(presenter.progressDialog()->labelText() == "Downloading \"parakeet-v3\"…");
    CHECK(presenter.progressDialog()->minimumWidth() == 360);
}

TEST_CASE("the presenter routes the GPU runtime offer back to ModelRuntime", "[widget]") {
    WindowHarness harness(Json{{"selected_model", "parakeet"}, {"hf_access_policy", "always"}});
    RuntimeHarness runtimes(harness.window.settings(), rtx2060());
    DownloadsPresenter presenter(harness.window.settings(), runtimes.ui, harness.window.io());
    presenter.host = [&runtimes] {
        return runtimes.host();
    };
    std::vector<std::string> shown;
    std::optional<std::string> pick = fvtest::kCudaRuntime;
    presenter.runDialog = [&](QDialog& dialog) {
        shown.emplace_back(dialog.metaObject()->className());
        if (auto* offer = qobject_cast<fv::GpuRuntimeDialog*>(&dialog)) {
            CHECK(offer->titleLabel()->text() == "Run Parakeet on the RTX 2060?");
            if (pick == fvtest::kCudaRuntime) {
                offer->gpuButton()->click();
            } else if (pick) {
                offer->cpuButton()->click();
            } else {
                offer->laterButton()->click();
            }
            return offer->result();
        }
        return static_cast<int>(QDialog::Rejected);
    };
    presenter.attach({runtimes.models.get(), runtimes.components.get(), nullptr, nullptr});

    SECTION("Use this GPU installs the GPU runtime") {
        runtimes.models->request_download(kParakeet, false);
        runtimes.ui.run_all();
        CHECK(shown == std::vector<std::string>{"fv::GpuRuntimeDialog"});
        CHECK(runtimes.coordinator->is_installing(fvtest::kCudaRuntime));
        CHECK_FALSE(runtimes.coordinator->is_installing(fvtest::kCpuRuntime));
    }
    SECTION("Later installs nothing and the download goes ahead") {
        pick.reset();
        runtimes.models->request_download(kParakeet, false);
        runtimes.ui.run_all();
        CHECK(shown == std::vector<std::string>{"fv::GpuRuntimeDialog"});
        CHECK_FALSE(runtimes.coordinator->is_any_installing());
        CHECK(presenter.progressDialog() != nullptr);
    }
}

TEST_CASE("the consent dialog's Open Settings opens Settings on the download policy", "[widget]") {
    // "Never connect" offers Open Settings in place of Always allow.
    WindowHarness harness(Json{{"selected_model", "parakeet"}, {"hf_access_policy", "never"}});
    RuntimeHarness runtimes(harness.window.settings());
    DownloadsPresenter presenter(harness.window.settings(), runtimes.ui, harness.window.io());
    presenter.setPageOptions([&harness] { return harness.pages.options(); });
    presenter.host = [&runtimes] {
        return runtimes.host();
    };
    presenter.runDialog = [](QDialog& dialog) {
        if (auto* consent = qobject_cast<fv::HuggingFaceConsentDialog*>(&dialog)) {
            fvtest::buttonText(*consent, QStringLiteral("Open Settings"))->click();
            return consent->result();
        }
        return static_cast<int>(QDialog::Rejected);
    };
    presenter.attach(
        {runtimes.models.get(), runtimes.components.get(), harness.settingsWindow.get(), nullptr});

    runtimes.models->request_download(kParakeet, false);
    runtimes.ui.run_all();

    SettingsDialog* dialog = harness.settingsWindow->dialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->currentDestination() == downloadsKey());
    CHECK(presenter.progressDialog() == nullptr);
    // The request slot was released: nothing is in flight.
    CHECK(runtimes.access->requests_in_flight() == 0);
}

TEST_CASE("the page's requests reach the runtimes", "[widget]") {
    WindowHarness harness(Json{{"selected_model", "parakeet"}, {"hf_access_policy", "ask"}});
    RuntimeHarness runtimes(harness.window.settings());
    harness.presenter.host = [&runtimes] {
        return runtimes.host();
    };
    std::vector<std::string> shown;
    harness.presenter.runDialog = [&shown](QDialog& dialog) {
        shown.emplace_back(dialog.metaObject()->className());
        return static_cast<int>(QDialog::Rejected);
    };
    harness.attach({runtimes.models.get(), runtimes.components.get(), nullptr, nullptr});
    SettingsDialog& dialog = harness.settingsWindow->ensureDialog();
    dialog.selectDestination(downloadsKey());
    DownloadsPage* page = harness.page();
    REQUIRE(page != nullptr);

    // Download asks for consent under "Ask before downloading" (Cancel: nothing starts).
    page->row(q(kOrukeet))->downloadButton()->click();
    runtimes.ui.run_all();
    CHECK(shown == std::vector<std::string>{"fv::HuggingFaceConsentDialog"});
    CHECK_FALSE(runtimes.coordinator->is_any_installing());

    // Install on a component row starts that install.
    page->componentRow(q(fvtest::kCudaRuntime))->installButton()->click();
    CHECK(runtimes.coordinator->is_installing(fvtest::kCudaRuntime));
    CHECK(runtimes.engine.released() == std::vector<std::string>{fvtest::kCudaRuntime});
}

TEST_CASE("model events reach the page, the rail and Quick Record", "[widget]") {
    WindowHarness harness;
    RuntimeHarness runtimes(harness.window.settings());
    auto record =
        std::make_shared<fv::RecordWidgets>(harness.window.settings(), harness.window.ui());
    fv::QuickRecordTab* tab = record->quickRecord();
    DownloadsPresenter presenter(harness.window.settings(), runtimes.ui, harness.window.io());
    presenter.setPageOptions([&harness] { return harness.pages.options(); });
    presenter.setDownloadShownElsewhere([](const QString&) { return true; });
    presenter.attach({runtimes.models.get(), runtimes.components.get(),
                      harness.settingsWindow.get(), record.get()});
    SettingsDialog& dialog = harness.settingsWindow->ensureDialog();
    std::vector<QString> started;
    QObject::connect(&presenter, &DownloadsPresenter::modelDownloadStarted,
                     [&started](const QString& model) { started.push_back(model); });

    runtimes.models->model_download_started.emit(kParakeet);
    runtimes.ui.run_all();
    CHECK(started == std::vector<QString>{q(kParakeet)});
    CHECK(tab->engineLoading());
    REQUIRE(presenter.page() != nullptr);
    CHECK(presenter.page()->messageText() == "Downloading \"parakeet-v3\"…");

    // Progress arrives throttled at the source; each one reaches the page and the rail.
    runtimes.models->model_download_progress.emit(kParakeet, 357'000'000, 714'000'000);
    runtimes.ui.run_all();
    CHECK(dialog.rail()->value(downloadsKey()) == "Downloading Parakeet TDT 0.6B v3 · 50%");
    CHECK(presenter.page()->messageText() ==
          q("Downloading \"parakeet-v3\"… " + fv::format_size_bytes(357'000'000) + " of " +
            fv::format_size_bytes(714'000'000)));

    runtimes.models->model_download_finished.emit(kParakeet, true);
    runtimes.ui.run_all();
    CHECK_FALSE(tab->engineLoading());
    CHECK(presenter.page()->messageText().isEmpty());
    CHECK(dialog.rail()->value(downloadsKey()).startsWith(QStringLiteral("0 of 2")));

    runtimes.models->model_deleted.emit(kParakeet, false,
                                        "A download for this model is in progress");
    runtimes.ui.run_all();
    CHECK(presenter.page()->messageText() ==
          "Could not delete: A download for this model is in progress");

    runtimes.models->batch_download_planned.emit({kParakeet, kOrukeet});
    runtimes.ui.run_all();
    CHECK(presenter.page()->messageText() == "Downloading 2 models…");
    runtimes.models->batch_download_finished.emit(2, 2);
    runtimes.ui.run_all();
    CHECK(presenter.page()->messageText() == "Downloaded 2 models");
}

TEST_CASE("component events reach the page's rows and message", "[widget]") {
    WindowHarness harness;
    RuntimeHarness runtimes(harness.window.settings());
    harness.attach({runtimes.models.get(), runtimes.components.get(), nullptr, nullptr});
    // The presenter's signals are delivered on the context's GUI executor here.
    SettingsDialog& dialog = harness.settingsWindow->ensureDialog();
    dialog.selectDestination(downloadsKey());
    DownloadsPage* page = harness.page();
    REQUIRE(page != nullptr);
    harness.pages.installing.insert(fvtest::kCudaRuntime);
    page->refreshComponents();
    std::vector<QString> finished;
    QObject::connect(
        &harness.presenter, &DownloadsPresenter::componentInstallFinished,
        [&finished](const QString& id, bool, const QString&) { finished.push_back(id); });

    harness.presenter.onComponentProgress(q(fvtest::kCudaRuntime), QStringLiteral("verifying"), 10,
                                          100);
    CHECK(page->componentRow(q(fvtest::kCudaRuntime))->badge()->text() == "Verifying…");

    harness.pages.installing.clear();
    harness.pages.components = {
        fvtest::runtimeInfo(fvtest::kCpuRuntime),
        fvtest::runtimeInfo(fvtest::kCudaRuntime, fv::models::ComponentState::Installed)};
    harness.presenter.onComponentInstallFinished(
        q(fvtest::kCudaRuntime), true,
        QStringLiteral("Restart OpenWhisper_CPP to switch to the GPU runtime."));
    CHECK(page->messageText() == "Restart OpenWhisper_CPP to switch to the GPU runtime.");
    CHECK(page->tabs()->buttons()[1]->titleLabel()->text() == "Components · 1 of 2 installed");
    CHECK(finished == std::vector<QString>{q(fvtest::kCudaRuntime)});

    harness.pages.components = {fvtest::runtimeInfo(fvtest::kCpuRuntime),
                                fvtest::runtimeInfo(fvtest::kCudaRuntime)};
    harness.presenter.onComponentStateChanged();
    CHECK(page->tabs()->buttons()[1]->titleLabel()->text() == "Components · 0 of 2 installed");
}

TEST_CASE("the fallback progress dialog shows only when Settings is hidden", "[widget]") {
    WindowHarness harness;
    harness.attach();

    SECTION("no Settings window") {
        harness.presenter.onModelDownloadStarted(q(kParakeet));
        QProgressDialog* progress = harness.presenter.progressDialog();
        REQUIRE(progress != nullptr);
        CHECK(progress->isVisible());
        CHECK(progress->maximum() == 0);
        harness.presenter.onModelDownloadProgress(q(kParakeet), 0, 0);
        CHECK(progress->maximum() == 0);
        harness.presenter.onModelDownloadProgress(q(kParakeet), 25, 100);
        CHECK(progress->maximum() == 100);
        CHECK(progress->value() == 25);
        CHECK(progress->labelText() ==
              q("Downloading \"parakeet-v3\"… " + fv::format_size_bytes(25) + " of " +
                fv::format_size_bytes(100)));
        harness.presenter.onModelDownloadFinished(q(kParakeet), true);
        CHECK(harness.presenter.progressDialog() == nullptr);
    }
    SECTION("Settings hidden") {
        harness.settingsWindow->ensureDialog();
        harness.presenter.onModelDownloadStarted(q(kParakeet));
        CHECK(harness.presenter.progressDialog() != nullptr);
        harness.presenter.onModelDownloadFinished(q(kParakeet), false);
        CHECK(harness.presenter.progressDialog() == nullptr);
    }
    SECTION("Settings shown") {
        harness.settingsWindow->ensureDialog().show();
        harness.presenter.onModelDownloadStarted(q(kParakeet));
        CHECK(harness.presenter.progressDialog() == nullptr);
        harness.settingsWindow->dialog()->hide();
    }
    SECTION("the guided setup shows it") {
        harness.presenter.setDownloadShownElsewhere(
            [](const QString& model) { return model == "parakeet-v3"; });
        harness.presenter.onModelDownloadStarted(q(kParakeet));
        CHECK(harness.presenter.progressDialog() == nullptr);
        harness.presenter.onModelDownloadStarted(q(kOrukeet));
        CHECK(harness.presenter.progressDialog() != nullptr);
        harness.presenter.onModelDownloadFinished(q(kOrukeet), true);
    }
}

// settings_dialog.py _on_hf_policy_changed -> on_hf_policy_changed (runtime/models.py): the saved
// policy reaches ModelRuntime, where "Always allow downloads" fetches the missing selected model.
TEST_CASE("choosing Always allow on the page fetches the missing selected model", "[widget]") {
    WindowHarness harness(Json{{"selected_model", "parakeet"}, {"hf_access_policy", "ask"}});
    RuntimeHarness runtimes(harness.window.settings());
    runtimes.engine.set_selected_backend("parakeet");
    harness.presenter.host = [&runtimes] {
        return runtimes.host();
    };
    std::vector<std::string> shown;
    harness.presenter.runDialog = [&shown](QDialog& dialog) {
        shown.emplace_back(dialog.metaObject()->className());
        return static_cast<int>(QDialog::Rejected);
    };
    harness.attach({runtimes.models.get(), runtimes.components.get(), nullptr, nullptr});
    SettingsDialog& dialog = harness.settingsWindow->ensureDialog();
    dialog.selectDestination(downloadsKey());
    auto* combo = qobject_cast<QComboBox*>(dialog.control("hf_policy_combo"));
    REQUIRE(combo != nullptr);
    runtimes.ui.run_all();
    QCoreApplication::processEvents();
    // Loading the page saves nothing and asks nothing.
    CHECK(combo->currentData().toString() == "ask");
    CHECK(shown.empty());
    CHECK(runtimes.access->requests_in_flight() == 0);

    // Never connect: saved, nothing fetched.
    fvtest::pick(combo, "never");
    runtimes.ui.run_all();
    QCoreApplication::processEvents();
    CHECK(fv::models::load_hf_access_policy(harness.window.settings()) == "never");
    CHECK(shown.empty());
    CHECK(runtimes.access->requests_in_flight() == 0);

    // Always allow: the selected model's own flow starts without a consent question; its
    // missing CPU runtime is asked about first.
    fvtest::pick(combo, "always");
    runtimes.ui.run_all();
    QCoreApplication::processEvents();
    CHECK(fv::models::load_hf_access_policy(harness.window.settings()) == "always");
    CHECK(shown == std::vector<std::string>{"fv::RequiredRuntimeDialog"});
}

TEST_CASE("the presenter answers engine_downloads with ModelRuntime's missing runtime",
          "[widget]") {
    WindowHarness harness(Json{{"selected_model", "parakeet"}});
    RuntimeHarness runtimes(harness.window.settings());
    harness.attach({runtimes.models.get(), runtimes.components.get(), nullptr, nullptr});
    runtimes.engine.set_selected_backend("parakeet");

    harness.settingsWindow->open(QStringLiteral("engine_downloads"));

    SettingsDialog* dialog = harness.settingsWindow->dialog();
    REQUIRE(dialog != nullptr);
    CHECK(dialog->currentDestination() == downloadsKey());
    // Parakeet on Auto with no GPU and nothing installed lacks the CPU runtime.
    REQUIRE(runtimes.models->missing_local_runtime() == fvtest::kCpuRuntime);
    CHECK(harness.page()->tabs()->currentIndex() == 1);
    CHECK(harness.page()
              ->componentRow(q(fvtest::kCpuRuntime))
              ->property(fv::ui::prop::selected)
              .toBool());
}
