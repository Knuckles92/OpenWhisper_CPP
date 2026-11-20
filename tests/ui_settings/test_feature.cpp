// The Settings feature: SettingsWindow opens the one dialog from the main window's requests
// (UIController's open_settings_dialog, open_settings_destination, open_engine_help_destination,
// open_hotkey_settings and open_downloads), and "SettingsFeature connects every signal"
// (architecture 8.5).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <QAction>
#include <QComboBox>
#include <QMetaMethod>
#include <QPointer>
#include <QVBoxLayout>

#include "audio/devices.h"
#include "base/executor.h"
#include "base/testing/map_env_reader.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "settings_support.h"
#include "ui_settings/api_keys_section.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_feature.h"
#include "ui_settings/settings_window.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"

using fvtest::qs;
using fvtest::SettingsRig;
namespace dest = fv::destinations;

namespace {

// A page that records its deep links and names the controls the routes reveal.
class LinkedPage : public QWidget, public fv::SettingsPageModule, public fv::SettingsDeepLinks {
public:
    LinkedPage(QStringList& calls, QWidget* parent) : QWidget(parent), calls_(calls) {
        auto* layout = new QVBoxLayout(this);
        tile = new fv::InfoTile(QStringLiteral("Cleanup model"), QString());
        layout->addWidget(tile);
    }
    void focusCredential(const QString& env) override { calls_.append("key " + env); }
    void showBackend(const QString& backend) override { calls_.append("backend " + backend); }
    void focusComponent(const QString& id) override { calls_.append("component " + id); }
    QWidget* namedControl(std::string_view name) override {
        return name == "cleanup_model_tile" ? tile : nullptr;
    }

    fv::InfoTile* tile;

private:
    QStringList& calls_;
};

// The destinations the Settings feature registers itself (AppPages, Backup, Fast judgments and
// Walkthroughs).
bool ownPage(std::string_view key) {
    return key == dest::recording || key == dest::general || key == dest::appearance ||
           key == dest::hotkeys || key == dest::api_keys || key == dest::advanced ||
           key == dest::backup || key == dest::meeting_fast || key == dest::walkthroughs;
}

// Stand-ins for the destinations no feature of this test registers.
void registerOtherStandIns(SettingsRig& rig,
                           const std::function<void(fv::SettingsPageSpec&)>& customize = {}) {
    for (const fvtest::Heading& heading : fvtest::headings()) {
        if (ownPage(heading.key)) {
            continue;
        }
        fv::SettingsPageSpec spec = fvtest::standInSpec(heading);
        if (customize) {
            customize(spec);
        }
        rig.registry().addPage(std::move(spec));
    }
}

// The services the App pages need that SettingsRig's context has not: an in-memory credential
// store and a worker.
struct PageServices {
    explicit PageServices(fv::AppContext& context) {
        fv::AppPagesHooks& hooks = context.service<fv::AppPages>().hooks();
        hooks.credentials = &store;
        hooks.env = &env;
        hooks.worker = &pool;
        hooks.listDevices = [] {
            return std::vector<fv::audio::InputDevice>{};
        };
    }
    ~PageServices() { pool.shutdown(std::chrono::seconds(5)); }

    fv::CredentialStore store{std::make_shared<fv::MemoryCredentialBackend>(), fv::identity()};
    fv::MapEnvReader env;
    fv::ThreadPool pool{"settings-feature-test", 2};
};

// The shell and the Settings feature installed over a SettingsRig's context, with every other
// destination a stand-in and the AI cleanup and Downloads pages linked.
struct FeatureRig {
    FeatureRig() {
        registerOtherStandIns(rig, [this](fv::SettingsPageSpec& spec) {
            if (spec.destination == dest::cleanup || spec.destination == dest::downloads) {
                if (spec.destination == dest::cleanup) {
                    spec.controls = {"cleanup_model_tile"};
                }
                spec.create = [this](QWidget* parent) -> QWidget* {
                    return new LinkedPage(calls, parent);
                };
            }
        });
        shell.provide(rig.context());
        settings.provide(rig.context());
        services = std::make_unique<PageServices>(rig.context());
        shell.install(rig.context());
        settings.install(rig.context());
        // As Bootstrap does once every feature has registered: the menus are connected here.
        window().completeSetup();
        // Raised without showing: the test checks what was opened, not a window on a screen.
        settingsWindow().raiseDialog = [this](fv::SettingsDialog&) {
            ++raised;
        };
    }

    fv::MainWindow& window() { return rig.context().service<fv::MainWindow>(); }
    fv::SettingsWindow& settingsWindow() { return rig.context().service<fv::SettingsWindow>(); }
    QString current() { return settingsWindow().dialog()->currentDestination(); }

    ~FeatureRig() {
        // The window goes before the services its pages read.
        if (fv::SettingsDialog* dialog = settingsWindow().dialog()) {
            delete dialog;
        }
        services.reset();
    }

    SettingsRig rig;
    fv::ShellFeature shell;
    fv::SettingsFeature settings;
    std::unique_ptr<PageServices> services;
    QStringList calls;
    int raised = 0;
};

using Unused = std::map<std::string, std::string>;

// The dialog's signals no feature of this package connects, with who connects them; the
// feature forwards hotkeyCaptureSuspended to SettingsPagesSignals, which the controller (M2.28)
// connects, and the API keys rail re-reads its credentials on windowShown (as the Recording page
// rescans its recordings, and the M2.20 and M2.23 pages theirs).
const Unused kDialogUnused{
    {"settingsChanged", "M2.28 (the controller's _on_settings_page_changed)"},
    {"destinationChanged", "no consumer: the pages' and tests' trace"},
    {"closed", "M2.28 (the controller's Settings-closed glue)"},
};

struct SignalProbe : QObject {
    using QObject::isSignalConnected;
};

bool connected(const QObject& object, const QMetaMethod& signal) {
    return (object.*(&SignalProbe::isSignalConnected))(signal);
}

} // namespace

TEST_CASE("SettingsFeature connects every signal", "[widget]") {
    FeatureRig feature;
    fv::MainWindow& window = feature.window();
    for (const QMetaMethod& signal :
         {QMetaMethod::fromSignal(&fv::MainWindow::settingsRequested),
          QMetaMethod::fromSignal(&fv::MainWindow::settingsDestinationRequested),
          QMetaMethod::fromSignal(&fv::MainWindow::hotkeysRequested),
          QMetaMethod::fromSignal(&fv::MainWindow::engineHelpRequested)}) {
        INFO(signal.name().toStdString());
        CHECK(window.hasReceivers(signal));
    }

    window.openSettings();
    fv::SettingsDialog* dialog = feature.settingsWindow().dialog();
    REQUIRE(dialog != nullptr);
    const QMetaObject& meta = *dialog->metaObject();
    std::set<std::string> declared;
    for (int index = fv::SettingsDialog::staticMetaObject.methodOffset();
         index < meta.methodCount(); ++index) {
        const QMetaMethod method = meta.method(index);
        if (method.methodType() != QMetaMethod::Signal) {
            continue;
        }
        const std::string name = method.name().toStdString();
        declared.insert(name);
        INFO(name);
        if (kDialogUnused.contains(name)) {
            CHECK_FALSE(connected(*dialog, method));
        } else {
            CHECK(connected(*dialog, method));
        }
    }
    for (const auto& [name, owner] : kDialogUnused) {
        INFO(name << " (" << owner << ")");
        CHECK(declared.contains(name));
    }
}

TEST_CASE("File > Settings opens the window on the first registered page and reuses it",
          "[widget]") {
    FeatureRig feature;
    CHECK(feature.settingsWindow().dialog() == nullptr);
    CHECK_FALSE(feature.settingsWindow().isVisible());
    fv::SettingsDialog* created = nullptr;
    QObject::connect(&feature.settingsWindow(), &fv::SettingsWindow::dialogCreated,
                     [&created](fv::SettingsDialog* dialog) { created = dialog; });

    feature.window().openSettings();
    fv::SettingsDialog* dialog = feature.settingsWindow().dialog();
    REQUIRE(dialog != nullptr);
    CHECK(created == dialog);
    CHECK(dialog->parentWidget() == &feature.window());
    CHECK(feature.current() == qs(dest::overview));
    CHECK(feature.raised == 1);

    // The rail lists only registered pages under their chapters.
    CHECK(dialog->rail()->keys().size() == static_cast<qsizetype>(fvtest::headings().size()));

    dialog->selectDestination(qs(dest::general));
    feature.window().openSettings();
    CHECK(feature.settingsWindow().dialog() == dialog);
    CHECK(feature.current() == qs(dest::overview));
    CHECK(feature.raised == 2);
}

TEST_CASE("the window's requests open their destinations", "[widget]") {
    FeatureRig feature;
    // File > Downloads…, as the menu item asks for it.
    QAction* downloads = feature.rig.context().menus().action(fv::menu_id::downloads);
    REQUIRE(downloads != nullptr);
    downloads->trigger();
    CHECK(feature.current() == qs(dest::downloads));

    feature.window().openHotkeySettings();
    CHECK(feature.current() == qs(dest::hotkeys));

    Q_EMIT feature.window().settingsDestinationRequested(qs(dest::appearance));
    CHECK(feature.current() == qs(dest::appearance));
    CHECK(feature.raised == 3);
}

// from
// tests/test_settings_unified.py::TestControllerRouting::test_other_names_select_their_destination
TEST_CASE("test_other_names_select_their_destination", "[test_settings_unified][widget]") {
    FeatureRig feature;
    feature.settingsWindow().open(QStringLiteral("ondemand"));
    CHECK(feature.current() == qs(dest::voice_model));
    CHECK(feature.raised == 1);
}

TEST_CASE("the text alias scrolls to the cleanup model", "[test_settings_unified][widget]") {
    // from
    // tests/test_settings_unified.py::TestControllerRouting::test_text_alias_scrolls_to_the_cleanup_model
    FeatureRig feature;
    feature.settingsWindow().open(QStringLiteral("text"));
    CHECK(feature.current() == qs(dest::cleanup));
    QApplication::processEvents();
    auto* page =
        dynamic_cast<LinkedPage*>(feature.settingsWindow().dialog()->pageContent(dest::cleanup));
    REQUIRE(page != nullptr);
    CHECK(page->tile->property("searchHit").toBool());
    CHECK(feature.raised == 1);
}

TEST_CASE("engine downloads focuses the missing runtime", "[test_settings_unified][widget]") {
    // from
    // tests/test_settings_unified.py::TestControllerRouting::test_engine_downloads_focuses_the_missing_runtime
    FeatureRig feature;
    feature.settingsWindow().setMissingRuntimeProvider(
        [] { return QStringLiteral("asr-nvidia-cuda"); });
    feature.settingsWindow().open(QStringLiteral("engine_downloads"));
    CHECK(feature.current() == qs(dest::downloads));
    CHECK(feature.calls == QStringList{QStringLiteral("component asr-nvidia-cuda")});

    // A runtime the guided setup shows the install of opens nothing.
    feature.settingsWindow().dialog()->selectDestination(qs(dest::general));
    feature.settingsWindow().setRuntimeInstallShownElsewhere([](const QString&) { return true; });
    feature.settingsWindow().openDownloads(QStringLiteral("asr-nvidia-cuda"));
    CHECK(feature.current() == qs(dest::general));
}

// from tests/test_field_help.py::test_help_routes_to_the_named_page
TEST_CASE("test_help_routes_to_the_named_page", "[test_field_help][widget]") {
    const std::string destination = GENERATE("ondemand", "downloads", "api_keys", "remote_engine");
    INFO(destination);
    FeatureRig feature;
    Q_EMIT feature.window().engineHelpRequested(qs(destination));
    REQUIRE(feature.settingsWindow().dialog() != nullptr);
    CHECK(feature.raised == 1);
    if (destination == "api_keys") {
        CHECK(feature.current() == qs(dest::api_keys));
        auto* keys = dynamic_cast<fv::ApiKeysSection*>(
            feature.settingsWindow().dialog()->pageContent(dest::api_keys));
        REQUIRE(keys != nullptr);
        CHECK(keys->combo()->currentData().toString() == QStringLiteral("OPENAI_API_KEY"));
        CHECK(feature.calls.isEmpty());
    } else {
        CHECK(feature.current() == qs(dest::resolve_destination(destination)));
        CHECK(feature.calls.isEmpty());
    }
}

TEST_CASE("engine help with no Settings page opens nothing", "[widget]") {
    FeatureRig feature;
    feature.settingsWindow().openEngineHelp(QStringLiteral("remote_models"));
    CHECK(feature.settingsWindow().dialog() == nullptr);
    CHECK(feature.raised == 0);
}

TEST_CASE("a page that cannot be built still lets the window open", "[widget]") {
    // The requests are Qt slots: what a page's create throws must not leave them.
    SettingsRig rig;
    int builds = 0;
    registerOtherStandIns(rig, [&builds](fv::SettingsPageSpec& spec) {
        if (spec.destination == dest::dictionary || spec.destination == dest::snippets) {
            spec.create = [&builds](QWidget*) -> QWidget* {
                ++builds;
                throw std::runtime_error("broken page");
            };
        }
    });
    fv::ShellFeature shell;
    fv::SettingsFeature settings;
    shell.provide(rig.context());
    settings.provide(rig.context());
    PageServices services(rig.context());
    shell.install(rig.context());
    settings.install(rig.context());
    auto& window = rig.context().service<fv::MainWindow>();
    auto& settingsWindow = rig.context().service<fv::SettingsWindow>();
    int raised = 0;
    settingsWindow.raiseDialog = [&raised](fv::SettingsDialog&) {
        ++raised;
    };

    Q_EMIT window.settingsDestinationRequested(qs(dest::dictionary));
    REQUIRE(settingsWindow.dialog() != nullptr);
    CHECK(raised == 1);
    CHECK(builds == 1);
    CHECK_FALSE(settingsWindow.dialog()->isBuilt(dest::dictionary));
    CHECK(settingsWindow.dialog()->currentDestination() == qs(dest::overview));

    Q_EMIT window.settingsDestinationRequested(qs(dest::snippets));
    CHECK(raised == 2);
    CHECK(builds == 2);

    // The page is tried again on the next opening, and the others still open.
    Q_EMIT window.settingsDestinationRequested(qs(dest::appearance));
    CHECK(settingsWindow.dialog()->currentDestination() == qs(dest::appearance));
    CHECK(raised == 3);
}

TEST_CASE("the policy link opens Downloads on the Hugging Face policy", "[widget]") {
    FeatureRig feature;
    feature.settingsWindow().openHfPolicy();
    CHECK(feature.current() == qs(dest::downloads));
    CHECK(feature.raised == 1);
}

TEST_CASE("host models help uses the configured paired route or opens pairing settings",
          "[test_field_help][widget]") {
    FeatureRig feature;
    bool paired = true;
    int requests = 0;
    feature.settingsWindow().setRemoteModelsOpener([&](QWidget* parent) {
        CHECK(parent == &feature.window());
        ++requests;
        return paired;
    });
    Q_EMIT feature.window().engineHelpRequested(QStringLiteral("remote_models"));
    CHECK(requests == 1);
    CHECK(feature.settingsWindow().dialog() == nullptr);
    CHECK(feature.raised == 0);

    paired = false;
    Q_EMIT feature.window().engineHelpRequested(QStringLiteral("remote_models"));
    REQUIRE(feature.settingsWindow().dialog() != nullptr);
    CHECK(requests == 2);
    CHECK(feature.raised == 1);
    CHECK(feature.current() == qs(dest::remote_engine));
}
