// The first-run offer and the singleton window (ui_controller.py offer_guided_setup,
// _offer_guided_setup_now, open_guided_setup, _show_guided_setup), the controller tests of
// tests/test_guided_setup.py, and setup_speech (services/guided_setup.py speech_model_name and
// speech_model_downloaded).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include "appcore/guided_setup_state.h"
#include "appkit/app_context.h"
#include "appkit/feature.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "engine/runtime.h"
#include "guided_setup_support.h"
#include "models/catalog.h"
#include "models/speech_catalog.h"
#include "pyport/json.h"
#include "settings/store.h"
#include "settings/testing/memory_store.h"
#include "ui_settings/settings_backup.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_setup/openwhisper_import.h"
#include "ui_setup/runtime_setup.h"
#include "ui_setup/setup_feature.h"
#include "ui_setup/setup_speech.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::GuidedSetup;
using fv::GuidedSetupDialog;
using fv::Json;
using fvtest::SetupRig;
namespace state = fv::appcore::guided_setup_state;

// A fresh folder of the test's own under the system temp folder, removed at the end.
class Scratch {
public:
    Scratch() {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("fv-guided-setup-test-" + std::to_string(QCoreApplication::applicationPid()) +
                 "-" + std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    const std::filesystem::path& path() const { return path_; }

    fv::Paths paths() const {
        // Every variable the per-user folder is read from on some platform: with LOCALAPPDATA
        // alone, the components this fixture writes and removes would be the real ones on Linux.
        const fv::MapEnvReader env{{"OPENWHISPER_CPP_DATA_DIR", path_.string()},
                                   {"LOCALAPPDATA", path_.string()},
                                   {"XDG_DATA_HOME", path_.string()},
                                   {"HOME", path_.string()},
                                   {"USERPROFILE", path_.string()}};
        return fv::Paths::resolve(fv::identity(), env);
    }

private:
    std::filesystem::path path_;
};

void touch(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "{}";
}

// _controller: the controller's parts the offer reads, around one SetupRig's ports.
class OfferRig {
public:
    explicit OfferRig(const Json& values = Json::object())
        : rig_(values), offer_(rig_.store(), [this] { return rig_.ports; }) {
        offer_.setMainWindow(&window);
        offer_.hooks().modalActive = [this] {
            return modal;
        };
        offer_.hooks().raiseDialog = [this](GuidedSetupDialog& dialog) {
            ++raised;
            dialog.show();
        };
    }
    ~OfferRig() {
        if (offer_.dialog() != nullptr) {
            offer_.dialog()->hide();
        }
    }

    GuidedSetup& offer() { return offer_; }
    SetupRig& rig() { return rig_; }

    QWidget window;
    bool modal = false;
    int raised = 0;

private:
    SetupRig rig_;
    GuidedSetup offer_;
};

} // namespace

// from tests/test_guided_setup.py::test_controller_offer_opens_on_welcome_and_menu_opens_first_step
TEST_CASE("test_controller_offer_opens_on_welcome_and_menu_opens_first_step",
          "[test_guided_setup][widget]") {
    OfferRig rig;
    state::save_state(state::kPending, rig.rig().store());
    rig.window.show();
    rig.offer().offerNow();
    GuidedSetupDialog* setup = rig.offer().dialog();
    REQUIRE(setup != nullptr);
    CHECK(setup->isVisible());
    CHECK(setup->page() == GuidedSetupDialog::Welcome);
    setup->skipButton()->click();
    CHECK_FALSE(setup->isVisible());
    rig.offer().open();
    CHECK(rig.offer().dialog() == setup);
    CHECK(setup->page() == GuidedSetupDialog::Record);
    CHECK(rig.rig().saved("guided_setup_state") == "started");
}

// from
// tests/test_guided_setup.py::test_controller_reopens_singleton_without_changing_dictation_choices
TEST_CASE("test_controller_reopens_singleton_without_changing_dictation_choices",
          "[test_guided_setup][widget]") {
    OfferRig rig(Json{{"auto_paste", false}});
    rig.offer().open();
    GuidedSetupDialog* setup = rig.offer().dialog();
    REQUIRE(setup != nullptr);
    CHECK(setup->page() == GuidedSetupDialog::Record);
    setup->showPage(GuidedSetupDialog::Output);
    setup->nextButton()->click();
    CHECK(rig.rig().saved("guided_setup_state") == "completed");
    setup->nextButton()->click();
    CHECK_FALSE(setup->isVisible());
    rig.offer().open();
    CHECK(rig.offer().dialog() == setup);
    CHECK(setup->page() == GuidedSetupDialog::Record);
    CHECK(rig.rig().saved("auto_paste") == false);
    CHECK(rig.rig().saved("guided_setup_state") == "completed");
    CHECK_FALSE(state::should_invite(rig.rig().store()));
}

// from
// tests/test_guided_setup.py::test_offer_waits_for_startup_modal_and_never_reappears_after_skip
TEST_CASE("test_offer_waits_for_startup_modal_and_never_reappears_after_skip",
          "[test_guided_setup][widget]") {
    OfferRig rig;
    state::save_state(state::kPending, rig.rig().store());
    rig.window.show();
    rig.modal = true;
    rig.offer().offerNow();
    CHECK(rig.offer().offerTimer()->isActive());
    CHECK(rig.offer().offerTimer()->interval() == GuidedSetup::kOfferRetryMs);
    CHECK(rig.offer().dialog() == nullptr);
    state::save_state(state::kSkipped, rig.rig().store());
    rig.modal = false;
    rig.offer().offerTimer()->stop();
    rig.offer().offerNow();
    CHECK(rig.offer().dialog() == nullptr);
    CHECK_FALSE(rig.offer().offerTimer()->isActive());
}

// from tests/test_guided_setup.py::test_offer_waits_for_a_visible_main_window
TEST_CASE("test_offer_waits_for_a_visible_main_window", "[test_guided_setup][widget]") {
    const auto [visible, minimized] = GENERATE(table<bool, bool>({{false, false}, {true, true}}));
    CAPTURE(visible, minimized);
    OfferRig rig;
    state::save_state(state::kPending, rig.rig().store());
    if (minimized) {
        rig.window.showMinimized();
    } else if (visible) {
        rig.window.show();
    }
    rig.offer().offerNow();
    CHECK(rig.offer().dialog() == nullptr);
    CHECK(rig.offer().offerTimer()->isActive());
    CHECK(rig.offer().offerTimer()->interval() == GuidedSetup::kOfferRetryMs);
}

TEST_CASE("the offer starts 750 ms after the main window first shows, only while pending",
          "[test_guided_setup][widget]") {
    OfferRig rig;
    rig.offer().offer();
    CHECK_FALSE(rig.offer().offerTimer()->isActive());
    state::save_state(state::kPending, rig.rig().store());
    rig.offer().watchFirstShow(&rig.window);
    rig.window.show();
    CHECK(rig.offer().offerTimer()->isActive());
    CHECK(rig.offer().offerTimer()->interval() == GuidedSetup::kOfferDelayMs);
    REQUIRE(fvtest::pumpUntil([&] { return rig.offer().dialog() != nullptr; }));
    CHECK(rig.offer().dialog()->page() == GuidedSetupDialog::Welcome);
    CHECK(rig.raised == 1);
    // A visible window is not offered again.
    rig.offer().offerNow();
    CHECK(rig.raised == 1);
    // Showing the main window again does not restart the offer.
    rig.window.hide();
    rig.window.show();
    CHECK_FALSE(rig.offer().offerTimer()->isActive());
}

TEST_CASE("the service forwards dictation and downloads to the window once it exists",
          "[test_guided_setup][widget]") {
    OfferRig rig;
    // Before the window exists nothing is shown or claimed.
    rig.offer().dictationReceived(QStringLiteral("Hello there."), true);
    rig.offer().modelDownloadStarted(QStringLiteral("parakeet-v3"));
    CHECK_FALSE(rig.offer().showsDownload(QStringLiteral("parakeet-v3")));
    CHECK_FALSE(rig.offer().showsRuntimeInstall(QStringLiteral("asr-nvidia-cpu")));
    std::vector<QString> asked;
    rig.offer().setDownloadModel([&](const QString& model) { asked.push_back(model); });
    rig.rig().speech.ready = false;
    rig.offer().open();
    GuidedSetupDialog* setup = rig.offer().dialog();
    REQUIRE(setup != nullptr);
    setup->speechDownloadButton()->click();
    REQUIRE(asked.size() == 1);
    CHECK(asked.front() == QStringLiteral("parakeet-v3"));
    // Asked for here: the app's own started handler may run before the window's, and already
    // leaves this download to the window. Another model's is not this window's.
    CHECK(rig.offer().showsDownload(QStringLiteral("parakeet-v3")));
    CHECK_FALSE(rig.offer().showsDownload(QStringLiteral("parakeet-v2")));
    rig.offer().modelDownloadStarted(QStringLiteral("parakeet-v3"));
    CHECK(rig.offer().showsDownload(QStringLiteral("parakeet-v3")));
    CHECK_FALSE(rig.offer().showsDownload(QStringLiteral("parakeet-v2")));
    CHECK(rig.offer().showsRuntimeInstall(QStringLiteral("asr-nvidia-cpu")));
    rig.offer().modelDownloadProgress(QStringLiteral("parakeet-v3"), 1, 4);
    CHECK(setup->speechProgress()->value() == 25);
    rig.offer().modelDownloadFinished(QStringLiteral("parakeet-v3"), false);
    CHECK(setup->speechSummary()->text().contains(QStringLiteral("didn't finish")));
    // A failed download is settled: a later one of the same model is the app's to show.
    CHECK_FALSE(rig.offer().showsDownload(QStringLiteral("parakeet-v3")));
    setup->showPage(GuidedSetupDialog::Ready);
    rig.offer().dictationFailed(QStringLiteral("No speech detected"));
    REQUIRE(fvtest::waitForQueuedRefresh(*setup));
    CHECK(setup->tryNote()->text().contains(QStringLiteral("No speech detected")));
}

TEST_CASE("SetupFeature seeds guided_setup_state and provides GuidedSetup",
          "[test_guided_setup][widget]") {
    fvtest::SettingsRig settings(Json{{"auto_paste", false}});
    std::vector<std::unique_ptr<fv::Feature>> features;
    features.push_back(std::make_unique<fv::SetupFeature>());
    fv::FeatureRegistry::run(features, settings.context());
    CHECK(settings.context().find<fv::RuntimeSetup>() != nullptr);
    CHECK(settings.context().find<GuidedSetup>() != nullptr);
    // An existing settings file is an existing install.
    CHECK(settings.settings().snapshot()->at("guided_setup_state") == "existing");
    CHECK(settings.settings().snapshot()->at("auto_paste") == false);
}

// OpenWhisper seeds eligibility in bootstrap.main (ui_qt/bootstrap.py:312), before the database
// exists. SetupFeature::provide seeds it again, which keeps a state saved earlier; seeded only
// there, after the start-up has opened the database, a fresh data root reads as an existing
// install and is never offered setup.
TEST_CASE("a fresh data root is offered setup only when eligibility is seeded before the database",
          "[test_guided_setup][widget]") {
    const auto [seededFirst, database] =
        GENERATE(table<bool, bool>({{true, true}, {false, true}, {false, false}}));
    CAPTURE(seededFirst, database);
    const Scratch temp;
    const fv::Paths paths = temp.paths();
    fv::SettingsStore store(paths.user_file(fv::identity().settings_file));
    REQUIRE(store.is_new_install());
    fv::qt::QtUiExecutor ui(qApp);
    fv::AppServices services;
    services.identity = &fv::identity();
    services.paths = &paths;
    services.settings = &store;
    services.ui = &ui;
    fv::AppContext context(services);

    if (seededFirst) {
        state::initialize(store);
    }
    if (database) {
        // The database file Bootstrap::run opens before the features run.
        touch(paths.user_file(fv::identity().database_file));
    }
    std::vector<std::unique_ptr<fv::Feature>> features;
    features.push_back(std::make_unique<fv::SetupFeature>());
    fv::FeatureRegistry::run(features, context);
    REQUIRE(context.find<GuidedSetup>() != nullptr);
    const bool invited = seededFirst || !database;
    CHECK(state::should_invite(store) == invited);
    CHECK(store.snapshot()->at("guided_setup_state") ==
          Json(std::string(invited ? state::kPending : state::kExisting)));
}

// The Backup page's "Import from OpenWhisper…" (ui_settings, which cannot name ui_setup) reaches
// the feature's import through BackupImportOffer.
TEST_CASE("SetupFeature offers its OpenWhisper import to the Backup page",
          "[test_guided_setup][widget]") {
    const Scratch temp;
    const fv::Paths paths = temp.paths();
    fv::MemorySettingsStore store(Json::object());
    fv::qt::QtUiExecutor ui(qApp);
    // Never run: nothing looks for OpenWhisper's data.
    fv::ManualExecutor jobs;
    fv::AppServices services;
    services.identity = &fv::identity();
    services.paths = &paths;
    services.settings = &store;
    services.ui = &ui;
    services.jobs = &jobs;
    fv::AppContext context(services);
    std::vector<std::unique_ptr<fv::Feature>> features;
    features.push_back(std::make_unique<fv::SetupFeature>());
    fv::FeatureRegistry::run(features, context);

    fv::OpenWhisperImport* import = context.find<fv::OpenWhisperImport>();
    fv::BackupImportOffer* offer = context.find<fv::BackupImportOffer>();
    REQUIRE(import != nullptr);
    REQUIRE(offer != nullptr);
    CHECK_FALSE(offer->available());
    std::vector<bool> changes;
    QObject::connect(offer, &fv::BackupImportOffer::availabilityChanged, offer,
                     [&](bool available) { changes.push_back(available); });
    Q_EMIT import->availabilityChanged(true);
    CHECK(changes == std::vector<bool>{true});
    offer->open(nullptr);
    REQUIRE(import->dialog() != nullptr);
    CHECK(import->dialog()->isVisible());
    import->dialog()->reject();
}

// ---- setup_speech ----

// from tests/test_guided_setup.py::test_speech_status_is_unknown_for_engines_without_a_local_model
TEST_CASE("test_speech_status_is_unknown_for_engines_without_a_local_model",
          "[test_guided_setup]") {
    const std::string engine = GENERATE("api", "remote");
    CAPTURE(engine);
    const Scratch temp;
    const fv::Paths paths = temp.paths();
    const Json settings{{"selected_model", engine}};
    CHECK_FALSE(fv::setup_speech::speech_model_name(settings).has_value());
    CHECK_FALSE(fv::setup_speech::speech_model_downloaded(settings, paths).has_value());
}

// from tests/test_guided_setup.py::test_speech_status_counts_a_missing_runtime_as_not_downloaded
TEST_CASE("test_speech_status_counts_a_missing_runtime_as_not_downloaded", "[test_guided_setup]") {
    const auto tag = fv::models::platform_tag();
    if (!tag) {
        SKIP("This platform has no runtime components");
    }
    const Scratch temp;
    const fv::Paths paths = temp.paths();
    fv::models::Host host;
    host.platform = *tag;
    const Json settings{{"selected_model", "parakeet"},
                        {"local_asr_devices", {{"parakeet", "cpu"}}}};
    CHECK(fv::setup_speech::speech_model_name(settings) ==
          std::optional<std::string>("parakeet-v3"));
    // No runtime installed: not downloaded, whatever the model folder holds.
    CHECK(fv::setup_speech::speech_model_downloaded(settings, paths, host) ==
          std::optional<bool>(false));
    // The runtime installed, the model folder missing: not downloaded either.
    touch(paths.components_root() / "asr-nvidia-cpu" / ".installed");
    CHECK(fv::setup_speech::speech_model_downloaded(settings, paths, host) ==
          std::optional<bool>(false));
    // A runtime folder chosen by hand stands in for the component when it holds the runtime
    // library (as resolve_parakeet counts it); a folder that does not exist never does.
    const std::filesystem::path runtime = temp.path() / "runtime";
    touch(runtime / "bin" / fv::runtime_library_name());
    touch(runtime / "bin" / "ggml-cpu.dll");
    Json chosen = settings;
    chosen["fv_runtime_dir"] = fv::base_utf8::path_to_utf8(runtime);
    Json missing = settings;
    missing["fv_runtime_dir"] = fv::base_utf8::path_to_utf8(temp.path() / "no-runtime-here");
    std::filesystem::remove_all(paths.components_root());
    CHECK(fv::setup_speech::speech_model_downloaded(chosen, paths, host) ==
          std::optional<bool>(false));

    // A complete model folder: installed.json and every pinned file at its pinned size. The
    // files are only extended to that size, never written, which is all the check reads.
    const Json& spec = fv::models::artifacts("parakeet-v3");
    const std::filesystem::path folder = paths.speech_models_root() / "parakeet-v3";
    std::filesystem::create_directories(folder);
    for (const Json& file : spec.at("files")) {
        const std::filesystem::path part =
            folder / fv::base_utf8::path_from_utf8(file.at("name").get<std::string>());
        std::ofstream(part, std::ios::binary).close();
        std::filesystem::resize_file(part, file.at("size_bytes").get<std::uintmax_t>());
    }
    std::ofstream(folder / "installed.json", std::ios::binary) << fv::py::dumps(spec);
    // The model without its runtime is still not ready to dictate.
    CHECK(fv::setup_speech::speech_model_downloaded(settings, paths, host) ==
          std::optional<bool>(false));
    // With the runtime, or with a runtime folder chosen by hand: downloaded and ready.
    CHECK(fv::setup_speech::speech_model_downloaded(chosen, paths, host) ==
          std::optional<bool>(true));
    CHECK(fv::setup_speech::speech_model_downloaded(missing, paths, host) ==
          std::optional<bool>(false));
    touch(paths.components_root() / "asr-nvidia-cpu" / ".installed");
    CHECK(fv::setup_speech::speech_model_downloaded(settings, paths, host) ==
          std::optional<bool>(true));
    // A part cut short is an incomplete download.
    const std::filesystem::path first =
        folder / fv::base_utf8::path_from_utf8(spec.at("files")[0].at("name").get<std::string>());
    std::filesystem::resize_file(first, std::filesystem::file_size(first) - 1);
    CHECK(fv::setup_speech::speech_model_downloaded(settings, paths, host) ==
          std::optional<bool>(false));
}
