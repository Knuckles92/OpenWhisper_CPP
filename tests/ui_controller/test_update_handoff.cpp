// The moment the app hands an update over to the setup: OpenWhisper's
// tests/test_app_update_handoff.py (10 of 11; the macOS disk image has no Windows counterpart) and
// two functions of tests/test_update_resilience.py. The setup waits for this process to exit, so a
// hand-off that leaves the app running is a hang the user cannot escape. Every exit, launch and
// watchdog here is a fake; the hand-off's native-updater branch is M10's, so the tests that drove
// it in Python drive the setup's.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QTimer>

#include "appcore/activity.h"
#include "base/testing/fake_clock.h"
#include "controller_support.h"
#include "settings/testing/memory_store.h"
#include "ui_controller/update_handoff.h"
#include "ui_controller/update_presenter.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"
#include "update/apply.h"
#include "update/gates.h"
#include "update/release_identity.h"
#include "update/testing/fake_mutex_api.h"
#include "update/update_copy.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using namespace std::chrono_literals;

namespace update = fv::update;

namespace {

update::UpdateCheckResult setupResult() {
    update::UpdateCheckResult result;
    result.status = update::UpdateStatus::UpdateAvailable;
    result.current_version = "0.1.0";
    result.channel = update::InstallChannel::Installer;
    update::ReleaseInfo release;
    release.version = "0.2.0";
    release.tag_name = "v0.2.0";
    release.html_url = "https://github.com/example/app/releases/tag/v0.2.0";
    update::ReleaseAsset setup;
    setup.name = "FioriVoce-Setup-0.2.0.exe";
    setup.url = "https://github.com/example/app/releases/download/v0.2.0/" + setup.name;
    setup.size_bytes = 4096;
    setup.sha256 = std::string(64, 'c');
    release.setup_asset = setup;
    result.release = release;
    result.can_apply = true;
    result.apply_mode = update::ApplyMode::Setup;
    return result;
}

update::Handoff setupHandoff() {
    update::InstallProbe probe;
    probe.registered = true;
    probe.install_dir = "C:/Custom Install/FioriVoce";
    probe.running_dir = "C:/Custom Install/FioriVoce";
    return update::setup_command("C:/User Data/updates/verified-setup.exe", probe,
                                 "C:/User Data/updates");
}

// A hand-off over recording ports, and the dialog a test opens.
struct Harness {
    fv::MemorySettingsStore settings;
    fv::ActivityState activity;
    std::vector<std::string> calls;
    std::vector<std::vector<std::string>> launches;
    bool launchSucceeds = true;
    bool answer = false;
    std::optional<update::UpdateCheckResult> last;
    std::unique_ptr<fv::AppUpdateDialog> dialog;
    std::unique_ptr<fv::UpdateHandoff> handoff;

    Harness() {
        fv::UpdateHandoffPorts ports;
        ports.dialog = [this] {
            return dialog.get();
        };
        ports.activity = [this] {
            return activity.snapshot();
        };
        ports.lastResult = [this] {
            return last;
        };
        ports.download = [this](const update::UpdateCheckResult&, bool forceSetup) {
            calls.push_back(forceSetup ? "download_setup" : "download");
        };
        ports.cancelDownload = [this] {
            calls.push_back("cancel_download");
        };
        ports.abandon = [this](const update::Handoff&) {
            calls.push_back("abandon");
        };
        ports.status = [this](const QString& status) {
            calls.push_back("status:" + status.toStdString());
        };
        ports.releaseForSetup = [this] {
            calls.push_back("release_mutex");
        };
        ports.reacquire = [this] {
            calls.push_back("reacquire_mutex");
            return true;
        };
        ports.startDetached =
            [this](const std::filesystem::path& program, std::span<const std::string> args,
                   const std::filesystem::path&) -> std::expected<std::uint32_t, std::string> {
            std::vector<std::string> launch{program.generic_string()};
            launch.insert(launch.end(), args.begin(), args.end());
            launches.push_back(launch);
            calls.push_back("start_detached");
            if (!launchSucceeds) {
                return std::unexpected("Could not start verified-setup.exe.");
            }
            return 77;
        };
        ports.armWatchdog = [this] {
            calls.push_back("watchdog");
        };
        ports.exitApp = [this](int code) {
            calls.push_back("exit:" + std::to_string(code));
        };
        ports.question = [this](QWidget*, const QString& title, const QString& text) {
            calls.push_back("question:" + title.toStdString() + ":" + text.toStdString());
            return answer;
        };
        ports.warning = [this](QWidget*, const QString& title, const QString& text) {
            calls.push_back("warning:" + title.toStdString() + ":" + text.toStdString());
        };
        handoff = std::make_unique<fv::UpdateHandoff>(std::move(ports));
    }

    fv::AppUpdateDialog& openDialog() {
        last = setupResult();
        dialog = std::make_unique<fv::AppUpdateDialog>(last, QString(), settings, true);
        return *dialog;
    }

    bool called(const std::string& call) const {
        return std::ranges::find(calls, call) != calls.end();
    }
    long count(const std::string& call) const { return std::ranges::count(calls, call); }
};

} // namespace

// from
// tests/test_app_update_handoff.py::TestNativeHandoff::test_handoff_exits_instead_of_asking_to_quit
// (the setup's hand-off: the native helper is M10's)
TEST_CASE("handoff exits instead of asking to quit", "[test_app_update_handoff][widget]") {
    Harness harness;
    fv::AppUpdateDialog& dialog = harness.openDialog();

    harness.handoff->onDownloadFinished(setupHandoff(), "");

    CHECK(harness.count("exit:0") == 1);
    CHECK(harness.count("watchdog") == 1);
    CHECK(harness.count("start_detached") == 1);
    // Marked handed off: the dialog no longer refuses to close.
    CHECK_FALSE(dialog.busy());
    CHECK_FALSE(dialog.laterButton()->isEnabled());
}

// from
// tests/test_app_update_handoff.py::TestNativeHandoff::test_failed_launch_keeps_the_app_running
TEST_CASE("failed launch keeps the app running", "[test_app_update_handoff][widget]") {
    Harness harness;
    fv::AppUpdateDialog& dialog = harness.openDialog();
    harness.launchSucceeds = false;

    harness.handoff->onDownloadFinished(setupHandoff(), "");

    CHECK_FALSE(harness.called("exit:0"));
    CHECK_FALSE(harness.called("watchdog"));
    CHECK(dialog.bodyLabel()->text() == QStringLiteral("The installer could not be started."));
}

// from
// tests/test_app_update_handoff.py::TestNativeHandoff::test_cancel_after_the_dialog_closed_does_not_pop_a_failure
TEST_CASE("cancel after the dialog closed does not pop a failure",
          "[test_app_update_handoff][widget]") {
    Harness harness;
    harness.handoff->requestCancel();

    harness.handoff->onDownloadFinished(std::nullopt, "The update was cancelled.");

    for (const std::string& call : harness.calls) {
        CHECK_FALSE(call.starts_with("warning:"));
    }
    CHECK(harness.called("status:Update canceled"));
}

// from tests/test_app_update_handoff.py::TestNativeHandoff::test_tray_exit_goes_through_the_window
TEST_CASE("tray exit goes through the window", "[test_app_update_handoff][widget]") {
    ControllerRig rig;
    int asked = 0;
    rig.window().confirmQuit = [&asked] {
        ++asked;
        return false;
    };
    Q_EMIT rig.tray().exitRequested();
    QApplication::processEvents();
    CHECK(asked == 1);
}

// from tests/test_app_update_handoff.py::TestNativeHandoff::test_a_user_quit_mid_task_asks_first
// (the Yes half asks the window's confirmation, not QApplication::quit, which would end the test
// process's event loop)
TEST_CASE("a user quit mid task asks first", "[test_app_update_handoff][widget]") {
    ControllerRig rig;
    rig.activity().set_transcribing(true);
    std::vector<QString> asked;
    bool answer = false;
    rig.ui().quitPrompt = [&asked, &answer](QWidget*, const fv::QuitQuestion& question) {
        asked.push_back(question.text);
        return answer;
    };
    bool quit = false;
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [&quit] { quit = true; });

    Q_EMIT rig.tray().exitRequested();
    QApplication::processEvents();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].contains(QStringLiteral("transcription")));
    CHECK_FALSE(quit);

    answer = true;
    CHECK(rig.window().confirmQuit());
    CHECK(asked.size() == 2);
}

// from
// tests/test_app_update_handoff.py::TestNativeHandoff::test_the_update_handoff_quits_without_asking_again
TEST_CASE("the update handoff quits without asking again", "[test_app_update_handoff][widget]") {
    ControllerRig rig;
    int asked = 0;
    rig.window().confirmQuit = [&asked] {
        ++asked;
        return false;
    };
    fv::UpdatePresenter* presenter = rig.feature().updatePresenter();
    REQUIRE(presenter != nullptr);

    presenter->handoff().exitForUpdate();

    CHECK(asked == 0);
    CHECK(rig.calls().has("watchdog"));
    CHECK(rig.calls().has("exit:0"));
}

// from
// tests/test_app_update_handoff.py::TestSetupHandoff::test_setup_fallback_skips_wizard_preserves_install_and_restarts
TEST_CASE("setup fallback skips wizard preserves install and restarts",
          "[test_app_update_handoff][widget]") {
    struct Scope {
        bool registered;
        bool allUsers;
        std::string scope;
    };
    for (const Scope& hive : {Scope{true, false, "/CURRENTUSER"}, Scope{true, true, "/ALLUSERS"},
                              Scope{false, false, "/CURRENTUSER"}}) {
        INFO(hive.scope << (hive.registered ? "" : " (no registration)"));
        Harness harness;
        harness.openDialog();
        update::InstallProbe probe;
        probe.registered = hive.registered;
        probe.all_users = hive.allUsers;
        probe.install_dir = "C:/Custom Install/FioriVoce";
        probe.running_dir = "C:/Custom Install/FioriVoce";
        const update::Handoff handoff =
            update::setup_command("verified-setup.exe", probe, "C:/User Data/updates");

        harness.handoff->onDownloadFinished(handoff, "");

        REQUIRE(harness.launches.size() == 1);
        const std::vector<std::string>& launch = harness.launches[0];
        CHECK(launch[0] == "verified-setup.exe");
        const auto has = [&launch](const std::string& arg) {
            return std::ranges::find(launch, arg) != launch.end();
        };
        CHECK((has("/SILENT") || has("/VERYSILENT")));
        CHECK(has("/SP-"));
        CHECK(has("/NORESTART"));
        CHECK(has("/FIORIVOCEUPDATE=1"));
        CHECK(has(hive.scope));
        CHECK_FALSE(has(hive.scope == "/ALLUSERS" ? "/CURRENTUSER" : "/ALLUSERS"));
        CHECK(std::ranges::any_of(launch, [](const std::string& arg) {
            return arg.starts_with("/DIR=") && arg.find("Custom Install") != std::string::npos;
        }));
        // The app mutexes go immediately before the setup starts.
        CHECK(harness.count("release_mutex") == 1);
        const auto release = std::ranges::find(harness.calls, "release_mutex");
        const auto start = std::ranges::find(harness.calls, "start_detached");
        CHECK(release < start);
        CHECK(harness.count("exit:0") == 1);
    }
}

// from
// tests/test_app_update_handoff.py::TestSetupHandoff::test_failed_setup_launch_reacquires_mutex_and_keeps_work_open
TEST_CASE("failed setup launch reacquires mutex and keeps work open",
          "[test_app_update_handoff][widget]") {
    Harness harness;
    fv::AppUpdateDialog& dialog = harness.openDialog();
    harness.launchSucceeds = false;

    harness.handoff->onDownloadFinished(setupHandoff(), "");

    CHECK(harness.count("reacquire_mutex") == 1);
    CHECK_FALSE(harness.called("exit:0"));
    CHECK(dialog.bodyLabel()->text() == QStringLiteral("The installer could not be started."));
    // offer_setup=False: no "Use installer instead".
    CHECK(dialog.setupButton()->isHidden());
}

// from tests/test_app_update_handoff.py::TestHandoffWatchdog::test_a_stalled_exit_is_forced
TEST_CASE("a stalled exit is forced", "[test_app_update_handoff]") {
    std::atomic<bool> fired{false};
    auto watchdog = fv::HandoffWatchdog::arm(50ms, [&fired] { fired = true; });
    CHECK(watchdog->waitStopped(5s));
    CHECK(fired);
}

// from
// tests/test_app_update_handoff.py::TestHandoffWatchdog::test_a_normal_exit_leaves_nothing_holding_the_process
TEST_CASE("a normal exit leaves nothing holding the process", "[test_app_update_handoff]") {
    std::atomic<bool> fired{false};
    auto watchdog = fv::HandoffWatchdog::arm(30s, [&fired] { fired = true; });
    CHECK(watchdog->running());
    watchdog->cancel();
    CHECK(watchdog->waitStopped(5s));
    CHECK_FALSE(watchdog->running());
    CHECK_FALSE(fired);
}

// from tests/test_update_resilience.py::test_ui_cancel_after_success_is_queued_keeps_app_open
TEST_CASE("ui cancel after success is queued keeps app open", "[test_update_resilience][widget]") {
    Harness harness;
    harness.openDialog();
    harness.handoff->requestCancel();
    CHECK(harness.called("cancel_download"));

    harness.handoff->onDownloadFinished(setupHandoff(), "");

    CHECK(harness.count("abandon") == 1);
    CHECK_FALSE(harness.called("exit:0"));
    CHECK(harness.dialog->bodyLabel()->text() == QStringLiteral("The update was canceled."));
}

// from tests/test_update_resilience.py::test_new_work_at_download_completion_can_decline_restart
TEST_CASE("new work at download completion can decline restart",
          "[test_update_resilience][widget]") {
    Harness harness;
    harness.openDialog();
    harness.activity.set_recording(true);
    harness.answer = false;

    harness.handoff->onDownloadFinished(setupHandoff(), "");

    CHECK(harness.count("abandon") == 1);
    CHECK_FALSE(harness.called("exit:0"));
    CHECK(harness.dialog->bodyLabel()->text() ==
          QStringLiteral("The update was not installed. Your work is still open."));
}

TEST_CASE("declining an update start while busy says it was not started", "[widget]") {
    Harness harness;
    harness.openDialog();
    harness.activity.set_component_installing(true);
    harness.activity.set_transcribing(true);

    harness.handoff->requestDownload(setupResult());

    // busy_reasons order: transcription before a component install.
    CHECK(harness.called("question:Install update?:FioriVoce is busy with transcription. Install "
                         "the update anyway? Unsaved work in that task may be lost."));
    CHECK_FALSE(harness.called("download"));
    CHECK(harness.dialog->bodyLabel()->text() == QStringLiteral("The update was not started."));

    harness.answer = true;
    harness.handoff->requestDownload(setupResult(), true);
    CHECK(harness.called("download_setup"));
}

TEST_CASE("a failed download without the dialog is a warning box", "[widget]") {
    Harness harness;
    harness.handoff->onDownloadFinished(std::nullopt, "The download failed.");
    CHECK(harness.called("warning:Update failed:The download failed."));
}

TEST_CASE("the hand-off's exit ends the event loop that runs", "[widget]") {
    // QCoreApplication::exit, the real default: no window can veto it, as one can veto quit().
    fv::UpdateHandoffPorts ports;
    fv::ActivityState activity;
    ports.activity = [&activity] {
        return activity.snapshot();
    };
    int armed = 0;
    ports.armWatchdog = [&armed] {
        ++armed;
    };
    fv::UpdateHandoff handoff(std::move(ports));
    QTimer::singleShot(0, [&handoff] { handoff.exitForUpdate(); });
    CHECK(QApplication::exec() == 0);
    CHECK(armed == 1);
}

// The controller hands the setup the app mutexes main() holds: released right before the launch,
// taken back when it fails (the launch gate over a fake mutex API, provided as main() provides it).
TEST_CASE("the controller releases and retakes the launch gate's mutexes", "[widget]") {
    fv::FakeClock clock;
    update::testing::FakeMutexApi mutexes(clock);
    update::InstallProbe installed;
    installed.registered = true;
    installed.install_dir = "C:/Programs/FioriVoce";
    installed.running_dir = "C:/Programs/FioriVoce";
    auto entered = update::LaunchGate::enter(update::release_identity(), installed, mutexes, clock);
    REQUIRE(entered.has_value());
    update::LaunchGate gate = std::move(*entered);
    REQUIRE(gate.holds_app_mutexes());

    bool launchSucceeds = false;
    bool heldAtLaunch = true;
    RigOptions options;
    options.beforeFeatures = [&gate](fv::AppContext& context) {
        context.provide(std::shared_ptr<update::LaunchGate>(&gate, [](update::LaunchGate*) {}));
    };
    options.platform = [&](fv::ControllerPlatform& platform) {
        platform.startDetached =
            [&](const std::filesystem::path&, std::span<const std::string>,
                const std::filesystem::path&) -> std::expected<std::uint32_t, std::string> {
            heldAtLaunch = gate.holds_app_mutexes();
            if (!launchSucceeds) {
                return std::unexpected("Could not start setup.exe.");
            }
            return 1;
        };
    };
    ControllerRig rig(options);
    fv::UpdateHandoff& handoff = rig.feature().updatePresenter()->handoff();

    handoff.onDownloadFinished(setupHandoff(), "");
    CHECK_FALSE(heldAtLaunch);
    CHECK(gate.holds_app_mutexes());
    CHECK_FALSE(rig.calls().has("exit:0"));
    CHECK(rig.calls().has("warning:Update failed:The installer could not be started."));

    launchSucceeds = true;
    handoff.onDownloadFinished(setupHandoff(), "");
    CHECK_FALSE(heldAtLaunch);
    CHECK_FALSE(gate.holds_app_mutexes());
    CHECK(rig.calls().has("exit:0"));
}
