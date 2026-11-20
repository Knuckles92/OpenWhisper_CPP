// The native updater that cannot be started (OpenWhisper's ui_qt/ui_controller.py
// _report_launch_failure, audit A16-25): the prepared transaction is abandoned, the dialog says
// "The updater could not be started." and still offers the verified setup, and nothing exits.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <QString>

#include "appcore/activity.h"
#include "settings/testing/memory_store.h"
#include "ui_controller/update_handoff.h"
#include "ui_setup/app_update_dialog.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"
#include "update/apply.h"
#include "update/check.h"

namespace update = fv::update;

namespace {

// A native-mode result whose release also has a verified setup to fall back to.
update::UpdateCheckResult nativeResult() {
    update::UpdateCheckResult result;
    result.status = update::UpdateStatus::UpdateAvailable;
    result.current_version = "0.1.0";
    result.channel = update::InstallChannel::Installer;
    update::ReleaseInfo release;
    release.version = "0.2.0";
    release.tag_name = "v0.2.0";
    update::ReleaseAsset setup;
    setup.name = "OpenWhisper_CPP-Setup-0.2.0.exe";
    setup.url = "https://github.com/example/app/releases/download/v0.2.0/" + setup.name;
    setup.size_bytes = 4096;
    setup.sha256 = std::string(64, 'c');
    release.setup_asset = setup;
    update::ReleaseAsset archive = setup;
    archive.name = "OpenWhisper_CPP-0.2.0-win-x64.zip";
    archive.sha256 = std::string(64, 'd');
    release.native_asset = archive;
    result.release = release;
    result.can_apply = true;
    result.apply_mode = update::ApplyMode::Native;
    return result;
}

struct NativeHarness {
    fv::MemorySettingsStore settings;
    fv::ActivityState activity;
    std::vector<std::string> calls;
    std::optional<update::UpdateCheckResult> last = nativeResult();
    std::unique_ptr<fv::AppUpdateDialog> dialog =
        std::make_unique<fv::AppUpdateDialog>(last, QString(), settings, true);
    std::unique_ptr<fv::UpdateHandoff> handoff;

    NativeHarness() {
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
        ports.abandon = [this](const update::Handoff& handoff) {
            calls.push_back("abandon:" + handoff.transaction_id);
        };
        ports.releaseForSetup = [this] {
            calls.push_back("release_mutex");
        };
        ports.reacquire = [this] {
            calls.push_back("reacquire_mutex");
            return true;
        };
        ports.startDetached =
            [this](const std::filesystem::path&, std::span<const std::string>,
                   const std::filesystem::path&) -> std::expected<std::uint32_t, std::string> {
            calls.push_back("start_detached");
            return std::unexpected("Could not start openwhisper-cpp-updater.exe.");
        };
        ports.armWatchdog = [this] {
            calls.push_back("watchdog");
        };
        ports.exitApp = [this](int code) {
            calls.push_back("exit:" + std::to_string(code));
        };
        ports.question = [](QWidget*, const QString&, const QString&) {
            return false;
        };
        ports.warning = [this](QWidget*, const QString&, const QString& text) {
            calls.push_back("warning:" + text.toStdString());
        };
        handoff = std::make_unique<fv::UpdateHandoff>(std::move(ports));
    }

    bool called(const std::string& call) const {
        return std::ranges::find(calls, call) != calls.end();
    }
};

} // namespace

// from ui_qt/ui_controller.py::_report_launch_failure, the transaction branch
TEST_CASE("an updater that cannot be started abandons the transaction and offers the setup",
          "[test_app_update_handoff][widget]") {
    NativeHarness harness;
    update::Handoff native;
    native.setup_exe = "C:/User Data/updates/tx/openwhisper-cpp-updater.exe";
    native.args = {"--transaction", std::string(32, 'a')};
    native.transaction_id = std::string(32, 'a');

    harness.handoff->onDownloadFinished(native, "");

    CHECK(harness.called("start_detached"));
    CHECK(harness.called("abandon:" + std::string(32, 'a')));
    CHECK_FALSE(harness.called("exit:0"));
    CHECK_FALSE(harness.called("watchdog"));
    CHECK(harness.dialog->bodyLabel()->text() ==
          QStringLiteral("The updater could not be started."));
    // offer_setup=True: the verified setup is still there to use.
    CHECK_FALSE(harness.dialog->setupButton()->isHidden());
}
