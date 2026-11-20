// The OpenWhisper import window and its service (ui_setup/openwhisper_import*.h), and the
// guided setup Welcome page's import row. OpenWhisper_CPP only: there is no OpenWhisper test to
// port and no OpenWhisper widget to take a reference from, so the committed references are widget
// trees generated from these widgets and reviewed by the lead (tests/reference/
// openwhisper_import_dialog/, tests/reference/guided_setup/welcome-import-*). The data folder is
// a copy of tests/fixtures/import/openwhisper; the backup coordinator runs over its fake
// service, the executors are manual, and nothing is adopted or staged for real.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QCheckBox>
#include <QCoreApplication>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>

#include "backup/coordinator.h"
#include "backup/import.h"
#include "backup/testing/fake_hooks.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "guided_setup_support.h"
#include "history/format_utils.h"
#include "models/component_details.h"
#include "screenshot/screenshot.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_setup/openwhisper_import.h"
#include "ui_setup/openwhisper_import_dialog.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fs = std::filesystem;
using fv::Json;
using fv::OpenWhisperImport;
using fv::OpenWhisperImportDialog;
using fv::backup::ImportOptions;
using fv::backup::ImportResult;
using fv::backup::testing::FakeBackupService;
using fv::backup::testing::FakeHooks;

namespace {

QString q(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::string runtimeId() {
    return fv::models::component_catalog().front().component_id;
}

// The OpenWhisper import over a copy of the fixture folder, a coordinator over the fake backup
// service, and the speech runtime and model the adopter would find.
struct ImportRig {
    explicit ImportRig(bool withCoordinator = true, const Json& settings = Json::object())
        : ui(clock), store(settings) {
        fs::copy(fv::testkit::source_root() / "tests" / "fixtures" / "import" / "openwhisper",
                 source, fs::copy_options::recursive);
        fv::MapEnvReader env{{"OPENWHISPER_CPP_DATA_DIR",
                              fv::base_utf8::path_to_utf8(temp.path() / "OpenWhisper_CPP")}};
        fv::pin_per_user_folders(env, temp.path() / "per-user");
        if (withCoordinator) {
            coordinator = std::make_unique<fv::backup::BackupCoordinator>(
                fv::Paths::resolve(fv::identity(), env), fv::identity(), hooks.hooks(), ui, worker,
                clock, "0.1.0", &service);
        }
        discovery.components.push_back({runtimeId(), temp.path() / "components", Json()});
        discovery.models.push_back(
            {"parakeet-v3", "Parakeet TDT 0.6B v3", temp.path() / "speech-models", 714});
        OpenWhisperImport::Ports ports;
        ports.findSources = [this] {
            std::vector<fv::backup::ImportSource> sources;
            if (sourceFound) {
                sources.push_back(*fv::backup::inspect_import_source(source));
            }
            return sources;
        };
        ports.inspectSource = [](const fs::path& root) {
            return fv::backup::inspect_import_source(root);
        };
        ports.running = [this](const fs::path&) {
            return running;
        };
        ports.discoverModels = [this] {
            return discovery;
        };
        ports.adopt = [this](const fv::models::Discovery& found) {
            adopted.push_back(found);
        };
        ports.coordinator = [this] {
            return coordinator.get();
        };
        ports.pickFolder = [this](QWidget*) {
            return picked;
        };
        import = std::make_unique<OpenWhisperImport>(store, ports, worker, ui);
    }

    ~ImportRig() {
        import.reset();
        coordinator.reset();
    }

    // The worker and the UI actor until neither has anything queued.
    void drain() {
        while (worker.run_all() + ui.run_all() > 0) {
        }
        QCoreApplication::processEvents();
    }

    OpenWhisperImportDialog& dialog() { return *import->dialog(); }

    fv::testkit::TempDataRoot temp;
    fs::path source = temp.path() / "OpenWhisper";
    fv::FakeClock clock;
    fv::ManualExecutor ui;
    fv::ManualExecutor worker;
    fv::MemorySettingsStore store;
    FakeBackupService service;
    FakeHooks hooks;
    std::unique_ptr<fv::backup::BackupCoordinator> coordinator;
    fv::models::Discovery discovery;
    std::vector<fv::models::Discovery> adopted;
    bool sourceFound = true;
    bool running = false;
    QString picked;
    std::unique_ptr<OpenWhisperImport> import;
};

std::vector<QString> expectedLines(const fs::path& root) {
    const auto* details = fv::models::find_component_details(runtimeId());
    return {
        QStringLiteral("Folder: ") + q(fv::base_utf8::path_to_utf8(root)),
        QStringLiteral("Settings: found"),
        QStringLiteral("History: 3 entries"),
        QStringLiteral("Recordings: ") + q(fv::format_size_bytes(18 + 60 + 52 + 43 + 12)),
        QStringLiteral("Speech runtime: ") + q(details->display_name),
        QStringLiteral("Speech model: Parakeet TDT 0.6B v3"),
    };
}

// The committed tree of a widget: generated from it and reviewed by the lead. With
// OPENWHISPER_CPP_WRITE_UI_SETUP_REFERENCES set the test writes the file instead of reading it;
// review the diff before committing it. Geometry and fonts are Windows' (the fonts of
// QT_QPA_FONTDIR), so the comparison runs there only.
void compareWithReference(Json tree, const fs::path& relative) {
    tree["geometry"][0] = 0;
    tree["geometry"][1] = 0;
    const fs::path file = fv::testkit::reference_path() / relative;
    if (fv::testkit::get_env("OPENWHISPER_CPP_WRITE_UI_SETUP_REFERENCES")) {
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << tree.dump(1) << '\n';
    }
#ifndef _WIN32
    SKIP("The reference tree has Windows' font metrics");
#endif
    REQUIRE(fs::exists(file));
    std::ifstream in(file, std::ios::binary);
    const Json expected = Json::parse(std::string(std::istreambuf_iterator<char>(in), {}));
    const auto differences = fv::screenshot::compare_trees(expected, tree);
    for (const auto& difference : differences) {
        UNSCOPED_INFO(difference);
    }
    CHECK(differences.empty());
}

// What the reference window shows: an install's folder with everything to bring over.
fv::ImportFindings referenceFindings() {
    fv::ImportFindings findings;
    fv::backup::ImportSource source;
    source.root = fv::base_utf8::path_from_utf8("C:\\Users\\you\\AppData\\Local\\OpenWhisper");
    source.has_settings = true;
    source.has_database = true;
    source.history_entries = 1234;
    source.recordings_bytes = 52428800;
    findings.source = source;
    findings.runtimes = {QStringLiteral("NeMo-Speech.cpp (NVIDIA CUDA)")};
    findings.models = {QStringLiteral("Parakeet TDT 0.6B v3")};
    return findings;
}

} // namespace

TEST_CASE("the import window lists the OpenWhisper folder it found", "[ui_setup][widget]") {
    ImportRig rig;
    rig.import->open(nullptr);
    REQUIRE(rig.import->dialog() != nullptr);
    OpenWhisperImportDialog& dialog = rig.dialog();
    CHECK(dialog.isVisible());
    CHECK(dialog.windowTitle() == "Import from OpenWhisper");
    CHECK(dialog.headerLabel()->text() == "Bring over your OpenWhisper data");
    // Before discovery answers.
    CHECK(dialog.summaryLines() == std::vector<QString>{"Looking for OpenWhisper's data…"});
    CHECK_FALSE(dialog.importButton()->isEnabled());

    rig.drain();

    CHECK(rig.import->available());
    CHECK(dialog.summaryLines() == expectedLines(rig.source));
    CHECK(dialog.importButton()->isEnabled());
    CHECK(dialog.importButton()->text() == "Import and restart");
    CHECK_FALSE(dialog.runningLabel()->isVisible());
    // The copy of API keys is the user's choice, off until ticked.
    CHECK_FALSE(dialog.copyKeysBox()->isChecked());
    CHECK(dialog.copyKeysBox()->text() == "Copy API keys");
    CHECK(rig.service.calls().empty());
    CHECK(rig.adopted.empty());
}

TEST_CASE("a running OpenWhisper is named in the window", "[ui_setup][widget]") {
    ImportRig rig;
    rig.running = true;
    rig.import->open(nullptr);
    rig.drain();
    CHECK(rig.dialog().runningLabel()->isVisible());
    CHECK(rig.dialog().runningLabel()->text() ==
          "OpenWhisper is running. Close OpenWhisper, then import.");
}

TEST_CASE("Import and restart adopts, stages through the coordinator and closes",
          "[ui_setup][widget]") {
    ImportRig rig;
    rig.import->open(nullptr);
    rig.drain();
    OpenWhisperImportDialog& dialog = rig.dialog();
    const bool tick = GENERATE(false, true);
    dialog.copyKeysBox()->setChecked(tick);

    dialog.importButton()->click();
    CHECK(dialog.busy());
    CHECK(dialog.progressBar()->isVisible());
    CHECK_FALSE(dialog.importButton()->isEnabled());
    CHECK_FALSE(dialog.laterButton()->isEnabled());
    dialog.reject(); // closing is refused while the import runs
    CHECK(dialog.isVisible());

    rig.drain();

    REQUIRE(rig.adopted.size() == 1);
    CHECK(rig.adopted[0].components.at(0).component_id == runtimeId());
    CHECK(rig.adopted[0].models.at(0).key == "parakeet-v3");
    CHECK(rig.service.calls() ==
          std::vector<std::vector<std::string>>{
              {"import", fv::base_utf8::path_to_utf8(rig.source), tick ? "true" : "false"}});
    CHECK_FALSE(dialog.isVisible());
    CHECK(dialog.result() == QDialog::Accepted);
    CHECK_FALSE(dialog.busy());
}

TEST_CASE("a refused import keeps the window open with the reason", "[ui_setup][widget]") {
    ImportRig rig;
    rig.service.on_import = [](const fs::path&, const ImportOptions&) -> ImportResult {
        throw fv::backup::OpenWhisperRunning(
            "OpenWhisper is running. Close OpenWhisper, then import again.");
    };
    rig.import->open(nullptr);
    rig.drain();
    rig.dialog().importButton()->click();
    rig.drain();
    CHECK(rig.dialog().isVisible());
    CHECK_FALSE(rig.dialog().busy());
    CHECK(rig.dialog().statusLabel()->isVisible());
    CHECK(rig.dialog().statusLabel()->text() ==
          "OpenWhisper is running. Close OpenWhisper, then import again.");
    CHECK(rig.dialog().importButton()->isEnabled());

    // A restore staged by someone else afterwards does not close it.
    rig.service.on_import = nullptr;
    rig.coordinator->prepare_restore(rig.source);
    rig.drain();
    CHECK(rig.dialog().isVisible());
}

TEST_CASE("without a backup coordinator nothing is staged", "[ui_setup][widget]") {
    ImportRig rig(false);
    rig.import->open(nullptr);
    rig.drain();
    rig.dialog().importButton()->click();
    rig.drain();
    CHECK(rig.adopted.empty());
    CHECK(rig.dialog().statusLabel()->text() == "Importing is not available right now.");
    CHECK(rig.dialog().isVisible());
}

TEST_CASE("no OpenWhisper data: another folder can be chosen", "[ui_setup][widget]") {
    ImportRig rig;
    rig.sourceFound = false;
    rig.import->open(nullptr);
    rig.drain();
    CHECK_FALSE(rig.import->available());
    CHECK(rig.dialog().summaryLines() ==
          std::vector<QString>{"No OpenWhisper data was found on this computer. Choose the "
                               "folder that holds openwhisper_settings.json."});
    CHECK_FALSE(rig.dialog().importButton()->isEnabled());

    rig.picked = q(fv::base_utf8::path_to_utf8(rig.temp.path() / "nothing"));
    rig.dialog().folderButton()->click();
    rig.drain();
    CHECK(rig.dialog().statusLabel()->text() == "No OpenWhisper data was found in that folder.");

    rig.picked = q(fv::base_utf8::path_to_utf8(rig.source));
    rig.dialog().folderButton()->click();
    rig.drain();
    CHECK(rig.dialog().summaryLines() == expectedLines(rig.source));
    CHECK(rig.dialog().importButton()->isEnabled());
    CHECK(rig.import->available());
}

TEST_CASE("the import is offered until one is recorded", "[ui_setup][widget]") {
    SECTION("found, nothing imported") {
        ImportRig rig;
        CHECK_FALSE(rig.import->offered()); // nothing discovered yet
        rig.import->refresh();
        rig.drain();
        CHECK(rig.import->offered());
    }
    SECTION("an fv_import without a time does not count") {
        ImportRig rig(true, Json::parse(R"({"fv_import": {"source": "C:/ow"}})"));
        rig.import->refresh();
        rig.drain();
        CHECK(rig.import->offered());
    }
    SECTION("imported before") {
        ImportRig rig(true, Json::parse(R"({"fv_import": {"source": "C:/ow",
            "at": "2026-11-15T10:00:00+00:00", "version": "0.1.0"}})"));
        rig.import->refresh();
        rig.drain();
        CHECK(rig.import->available());
        CHECK(rig.import->imported());
        CHECK_FALSE(rig.import->offered());
    }
    SECTION("nothing found") {
        ImportRig rig;
        rig.sourceFound = false;
        rig.import->refresh();
        rig.drain();
        CHECK_FALSE(rig.import->offered());
    }
}

TEST_CASE("the Welcome row shows only while the import is offered", "[ui_setup][widget]") {
    fvtest::SetupRig rig(Json::object(), true);
    // Without an import service the page is M2's: no row is ever made.
    CHECK(rig.setup().importRow() == nullptr);

    bool offered = false;
    std::vector<QWidget*> opened;
    rig.ports.importOffered = [&] {
        return offered;
    };
    rig.ports.openImport = [&](QWidget* parent) {
        opened.push_back(parent);
    };
    rig.rebuild(true);
    CHECK(rig.setup().importRow() == nullptr);
    CHECK(rig.setup().welcomeExtras()->count() == 0);

    offered = true;
    rig.setup().refreshImportRow();
    REQUIRE(rig.setup().importRow() != nullptr);
    CHECK(rig.setup().welcomeExtras()->count() == 1);
    rig.setup().show();
    QCoreApplication::processEvents();
    CHECK(rig.setup().importRow()->isVisible());
    const auto labels = rig.setup().importRow()->findChildren<QLabel*>();
    REQUIRE(labels.size() == 2);
    CHECK(labels[0]->text() == "Bring over your OpenWhisper data");
    CHECK(rig.setup().importButton()->text() == "Import…");
    rig.setup().importButton()->click();
    CHECK(opened == std::vector<QWidget*>{&rig.setup()});

    offered = false; // imported, or the folder went away
    rig.setup().refreshImportRow();
    CHECK_FALSE(rig.setup().importRow()->isVisible());

    // begin() asks again.
    offered = true;
    rig.setup().begin(true);
    CHECK(rig.setup().importRow()->isVisible());
}

TEST_CASE("the import window's tree, dark", "[ui_setup][widget]") {
    fvtest::AppTheme theme(QStringLiteral("dark"));
    OpenWhisperImportDialog dialog;
    dialog.setFindings(referenceFindings());
    dialog.show();
    QCoreApplication::processEvents();
    compareWithReference(fv::screenshot::dump_tree(dialog), "openwhisper_import_dialog/dark.json");
}

TEST_CASE("the import window's tree, light", "[ui_setup][widget]") {
    fvtest::AppTheme theme(QStringLiteral("light"));
    OpenWhisperImportDialog dialog;
    dialog.setFindings(referenceFindings());
    dialog.show();
    QCoreApplication::processEvents();
    compareWithReference(fv::screenshot::dump_tree(dialog), "openwhisper_import_dialog/light.json");
}

TEST_CASE("the theme changes the import window's colours and nothing in its tree",
          "[ui_setup][widget]") {
    Json trees[2];
    for (const int index : {0, 1}) {
        fvtest::AppTheme theme(index == 0 ? QStringLiteral("dark") : QStringLiteral("light"));
        OpenWhisperImportDialog dialog;
        dialog.setFindings(referenceFindings());
        dialog.show();
        QCoreApplication::processEvents();
        trees[index] = fv::screenshot::dump_tree(dialog);
    }
    CHECK(fv::screenshot::compare_trees(trees[0], trees[1]).empty());
}

namespace {

// The Welcome page with the import row, in the window at the references' 720x690.
void welcomeTree(const char* theme) {
    fvtest::AppTheme appTheme(QString::fromLatin1(theme));
    fvtest::SetupRig rig(Json::object(), true);
    rig.ports.importOffered = [] {
        return true;
    };
    rig.ports.openImport = [](QWidget*) {
    };
    rig.rebuild(true);
    fv::GuidedSetupDialog& setup = rig.setup();
    setup.resize(fv::GuidedSetupDialog::kDefaultSize);
    setup.show();
    QCoreApplication::processEvents();
    REQUIRE(setup.page() == fv::GuidedSetupDialog::Welcome);
    compareWithReference(
        fv::screenshot::dump_tree(*setup.pages()->widget(fv::GuidedSetupDialog::Welcome)),
        std::string("guided_setup/welcome-import-") + theme + ".json");
}

} // namespace

TEST_CASE("the Welcome page's tree with the import row, dark", "[ui_setup][widget]") {
    welcomeTree("dark");
}

TEST_CASE("the Welcome page's tree with the import row, light", "[ui_setup][widget]") {
    welcomeTree("light");
}

TEST_CASE("the guided setup's ports reach the context's import", "[ui_setup][widget]") {
    fvtest::SettingsRig settings;
    ImportRig rig;
    // The rig owns the service; the context only finds it.
    settings.context().provide(std::shared_ptr<OpenWhisperImport>(rig.import.get(), [](auto*) {}));
    const fv::GuidedSetupPorts ports = fv::GuidedSetup::portsFor(settings.context());
    REQUIRE(ports.importOffered);
    REQUIRE(ports.openImport);
    CHECK_FALSE(ports.importOffered());
    rig.import->refresh();
    rig.drain();
    CHECK(ports.importOffered());
    ports.openImport(nullptr);
    REQUIRE(rig.import->dialog() != nullptr);
    CHECK(rig.import->dialog()->isVisible());
    rig.import->dialog()->reject();
}
TEST_CASE("a folder the user chose is kept when the window opens again", "[ui_setup][widget]") {
    ImportRig rig;
    const fs::path other = rig.temp.path() / "Elsewhere";
    fs::copy(rig.source, other, fs::copy_options::recursive);
    rig.import->open(nullptr);
    rig.drain();
    REQUIRE(rig.dialog().summaryLines() == expectedLines(rig.source));

    rig.picked = q(fv::base_utf8::path_to_utf8(other));
    rig.dialog().folderButton()->click();
    rig.drain();
    REQUIRE(rig.dialog().summaryLines() == expectedLines(other));
    rig.dialog().reject();

    // Opening again looks again, at the chosen folder rather than the one found on its own.
    rig.import->open(nullptr);
    rig.drain();
    CHECK(rig.dialog().summaryLines() == expectedLines(other));
    rig.import->refresh();
    rig.drain();
    CHECK(rig.import->findings().source->root == other);

    // Once the chosen folder holds nothing to import, the automatic discovery's folder is back.
    fs::remove_all(other);
    rig.dialog().reject();
    rig.import->open(nullptr);
    rig.drain();
    CHECK(rig.dialog().summaryLines() == expectedLines(rig.source));
}

TEST_CASE("another caller's refusal while the import runs does not end it", "[ui_setup][widget]") {
    ImportRig rig;
    rig.import->open(nullptr);
    rig.drain();
    rig.dialog().importButton()->click();
    // Adopting, then prepare_import on the UI actor: the coordinator is now the import's.
    rig.worker.run_all();
    rig.ui.run_all();
    REQUIRE(rig.coordinator->busy());
    REQUIRE(rig.dialog().busy());

    rig.coordinator->prepare_restore(rig.source); // refused: "Another backup operation ..."
    rig.ui.run_all();

    CHECK(rig.dialog().busy());
    CHECK(rig.dialog().isVisible());

    rig.drain();

    CHECK(rig.service.calls() == std::vector<std::vector<std::string>>{
                                     {"import", fv::base_utf8::path_to_utf8(rig.source), "false"}});
    CHECK_FALSE(rig.dialog().isVisible());
    CHECK(rig.dialog().result() == QDialog::Accepted);
}
