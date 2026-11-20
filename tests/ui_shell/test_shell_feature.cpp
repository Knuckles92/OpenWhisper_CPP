// "ShellFeature connects every signal" (architecture 8.5): every signal MainWindow and SystemTray
// declare has a receiver once the shell is wired, or is listed below as intentionally unused in
// M0 with the package that connects it.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QMetaMethod>

#include "appkit/feature.h"
#include "shell_fixture.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_shell/system_tray.h"

namespace {

// Signals nothing in M0 receives, and who connects them.
const std::map<std::string, std::string> kUnusedWindowSignals{
    {"recordToggled", "M1.16"},
    {"recordCanceled", "M1.16"},
    {"walkthroughRequested", "M6"},
    {"walkthroughLibraryRequested", "M6"},
    {"walkthroughsChanged", "M6"},
    {"modelChanged", "M1.16"},
    {"whisperEngineChanged", "dropped engine, never emitted"},
    {"remoteModelSelected", "M9"},
    {"remoteRuntimeSelected", "M9"},
    {"remoteRetryRequested", "M9"},
    {"livePreviewChanged", "M2"},
    {"cleanupToggled", "M4"},
    {"settingsRequested", "M1.16 (Settings window M2)"},
    {"guidedSetupRequested", "M2"},
    {"settingsDestinationRequested", "M1.16 (Settings window M2)"},
    {"engineHelpRequested", "M1.16"},
    {"hotkeysRequested", "M1.16"},
    {"aboutRequested", "M1.16"},
    {"checkForUpdatesRequested", "M3"},
    {"updateNoticeRequested", "M3"},
    {"retranscribeRequested", "M1.16"},
    {"uploadFileRequested", "M2"},
    {"uploadFilesRequested", "M2"},
    {"uploadCancelRequested", "M2"},
    {"uploadCopyRequested", "M2"},
    {"quickRecordCopyRequested", "M1.16"},
    {"meetingDashboardRequested", "M7"},
    {"scratchpadRequested", "M2"},
    {"statsRequested", "M2"},
    {"hostModelSelected", "M9"},
    {"pastMeetingRequested", "M7"},
    {"pastMeetingCopyRequested", "M7"},
    {"pastMeetingDeleteRequested", "M7"},
    {"pastMeetingsClearRequested", "M7"},
    {"screenCreated", "M1.16"},
    {"sidePanelRefreshDue", "M1.15"},
    {"sidePanelPageRequested", "M1.15"},
    {"compactModeChanged", "M1.16"},
};

const std::map<std::string, std::string> kUnusedTraySignals{
    {"toggleRecording", "M1.16"},        {"meetingToggleRequested", "M7"},
    {"meetingDashboardRequested", "M7"}, {"copyLastOriginalRequested", "M1.16"},
    {"walkthroughRequested", "M6"},      {"screenshotRequested", "M6"},
    {"scratchpadRequested", "M2"},       {"statsRequested", "M2"},
    {"updateRequested", "M3"},           {"languageMenuRequested", "M1.16"},
};

// The signals a class declares itself (not its Qt bases').
template <class T> std::vector<QMetaMethod> ownSignals() {
    std::vector<QMetaMethod> found;
    const QMetaObject& meta = T::staticMetaObject;
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        if (meta.method(i).methodType() == QMetaMethod::Signal) {
            found.push_back(meta.method(i));
        }
    }
    return found;
}

template <class T>
void checkConnected(const T& object, const std::map<std::string, std::string>& unused) {
    std::set<std::string> declared;
    for (const QMetaMethod& signal : ownSignals<T>()) {
        const std::string name = signal.name().toStdString();
        declared.insert(name);
        INFO(T::staticMetaObject.className() << "::" << name);
        if (unused.contains(name)) {
            CHECK_FALSE(object.hasReceivers(signal));
        } else {
            CHECK(object.hasReceivers(signal));
        }
    }
    // Every listed signal exists, so the list cannot go stale.
    for (const auto& [name, owner] : unused) {
        INFO(name << " (" << owner << ")");
        CHECK(declared.contains(name));
    }
}

} // namespace

TEST_CASE("ShellFeature connects every signal", "[widget]") {
    fvtest::ShellFixture shell;
    auto window = shell.window();
    fv::TrayEnvironment environment;
    environment.available = [] {
        return true;
    };
    environment.showIcon = [](QSystemTrayIcon&) {
    };
    fv::SystemTray tray(*window, environment);
    fv::connectShell(*window, tray);

    checkConnected(*window, kUnusedWindowSignals);
    checkConnected(tray, kUnusedTraySignals);
}

TEST_CASE("every main window signal of main_window.py is declared", "[unit]") {
    // main_window.py:378-421, in camelCase.
    const std::vector<std::string> python{"recordToggled",
                                          "recordCanceled",
                                          "walkthroughRequested",
                                          "walkthroughLibraryRequested",
                                          "walkthroughsChanged",
                                          "modelChanged",
                                          "whisperEngineChanged",
                                          "remoteModelSelected",
                                          "remoteRuntimeSelected",
                                          "remoteRetryRequested",
                                          "livePreviewChanged",
                                          "cleanupToggled",
                                          "settingsRequested",
                                          "guidedSetupRequested",
                                          "settingsDestinationRequested",
                                          "engineHelpRequested",
                                          "hotkeysRequested",
                                          "aboutRequested",
                                          "checkForUpdatesRequested",
                                          "updateNoticeRequested",
                                          "retranscribeRequested",
                                          "uploadFileRequested",
                                          "uploadFilesRequested",
                                          "uploadCancelRequested",
                                          "uploadCopyRequested",
                                          "quickRecordCopyRequested",
                                          "meetingDashboardRequested",
                                          "scratchpadRequested",
                                          "statsRequested",
                                          "hostModelSelected",
                                          "pastMeetingRequested",
                                          "pastMeetingCopyRequested",
                                          "pastMeetingDeleteRequested",
                                          "pastMeetingsClearRequested",
                                          "screenCreated"};
    // The window's own host signals follow Python's.
    const std::vector<std::string> host{"sidePanelRefreshDue", "sidePanelPageRequested",
                                        "compactModeChanged"};
    std::vector<std::string> declared;
    for (const QMetaMethod& signal : ownSignals<fv::MainWindow>()) {
        declared.push_back(signal.name().toStdString());
    }
    std::vector<std::string> expected = python;
    expected.insert(expected.end(), host.begin(), host.end());
    CHECK(declared == expected);
}

TEST_CASE("the shell registers its feature id and the upload screen", "[widget]") {
    fvtest::ShellFixture shell;
    fv::ShellFeature feature;
    CHECK(feature.id() == "shell");
    CHECK(feature.required_features().empty());
    CHECK(shell.context().screens().isDefined(fv::ShellFeature::kUploadScreen));
    const auto screens = shell.context().screens().screens();
    REQUIRE(screens.size() == 1);
    CHECK(screens[0].order == 200);
    CHECK(screens[0].title == QStringLiteral("Upload File"));
}
