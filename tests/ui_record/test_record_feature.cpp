// RecordFeature: the Quick Record and Upload File screens in the registry, the widgets and the
// upload presenter it provides, and "the record feature connects every signal" (architecture
// 8.5): every signal of the tabs and the compact controller has a receiver once the feature is
// installed, or is listed here as left to the package that wires it.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QMetaMethod>
#include <QPointer>
#include <QWidget>

#include "app_theme.h"
#include "appkit/app_context.h"
#include "appkit/order.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "settings/testing/memory_store.h"
#include "ui_record/compact_record_controller.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_record/upload_file_tab.h"
#include "ui_record/upload_presenter.h"
#include "ui_shell/main_window.h"

namespace {

// Signals nothing in M1.9 receives, and who connects them.
const std::map<std::string, std::string> kUnusedTabSignals{
    {"recordToggled", "M1.16 (record and stop go to the recorder)"},
    {"recordCanceled", "M1.16"},
    {"copyRequested", "M1.16 (the clipboard service)"},
    {"modelChanged", "M1.16 (saves the choice and reloads the engine)"},
    {"engineSettingsChanged", "M1.16 (reloads the engine)"},
    {"helpRequested", "M1.16 (opens the Settings destination; the window is M2)"},
    {"engineDownloadsRequested", "M1.16 (opens Downloads; the page is M2)"},
    {"transcriptionCollapsed", "M1.16 (MainWindow::onTranscriptionCollapsed)"},
    {"livePreviewChanged", "M2.28 (RecordWidgets::refreshLivePreviewControls, the runtime)"},
    {"cleanupToggled", "M4 (the cleanup runtime)"},
    {"settingsRequested", "M1.16 (opens the Settings destination; the window is M2)"},
    {"profilesRequested", "M4 (the cleanup profile card)"},
    {"remoteModelSelected", "M9.11 (UiController::wireTranscriptionTab, the remote runtime)"},
    {"remoteRuntimeSelected", "M9.11 (UiController::wireTranscriptionTab, the remote runtime)"},
    {"remoteRetryRequested", "M9.11 (UiController::wireTranscriptionTab, the remote runtime)"},
};

// The Upload File tab's requests go to the presenter; the engine card's signals, like Quick
// Record's, are the controller's.
const std::map<std::string, std::string> kUnusedUploadSignals{
    {"modelChanged", "M2.28 (saves the choice and reloads the engine)"},
    {"engineSettingsChanged", "M2.28 (reloads the engine)"},
    {"helpRequested", "M2.28 (opens the Settings destination)"},
    {"engineDownloadsRequested", "M2.28 (opens Downloads)"},
    {"transcriptionCollapsed", "M2.28 (MainWindow::onTranscriptionCollapsed)"},
    {"cleanupToggled", "M4 (the cleanup runtime)"},
    {"remoteModelSelected", "M9.11 (UiController::wireTranscriptionTab, the remote runtime)"},
    {"remoteRuntimeSelected", "M9.11 (UiController::wireTranscriptionTab, the remote runtime)"},
    {"remoteRetryRequested", "M9.11 (UiController::wireTranscriptionTab, the remote runtime)"},
    {"livePreviewChanged", "M2.28 (RecordWidgets::refreshLivePreviewControls, the runtime)"},
    {"statsVisibilityChanged", "M2.28 (the window's page fit; the strip never shows here)"},
};

const std::map<std::string, std::string> kUnusedCompactSignals{
    {"recordRequested", "M1.16"},
    {"stopRequested", "M1.16"},
    {"cancelRequested", "M1.16"},
};

// The signals a class declares itself (not its Qt bases').
std::vector<QMetaMethod> ownSignals(const QMetaObject& meta) {
    std::vector<QMetaMethod> found;
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        if (meta.method(i).methodType() == QMetaMethod::Signal) {
            found.push_back(meta.method(i));
        }
    }
    return found;
}

// Every signal of the class and of its bases below QWidget.
template <class T>
void checkConnected(const T& object, const QMetaObject* meta,
                    const std::map<std::string, std::string>& unused) {
    std::set<std::string> declared;
    for (const QMetaObject* level = meta; level != &QWidget::staticMetaObject;
         level = level->superClass()) {
        for (const QMetaMethod& signal : ownSignals(*level)) {
            const std::string name = signal.name().toStdString();
            declared.insert(name);
            INFO(level->className() << "::" << name);
            if (unused.contains(name)) {
                CHECK_FALSE(object.hasReceivers(signal));
            } else {
                CHECK(object.hasReceivers(signal));
            }
        }
    }
    // Every listed signal exists, so the list cannot go stale.
    for (const auto& [name, owner] : unused) {
        INFO(name << " (" << owner << ")");
        CHECK(declared.contains(name));
    }
}

// What Bootstrap builds for the feature: an in-memory settings file, the GUI executor and an
// AppContext with the feature installed.
class RecordFixture {
public:
    RecordFixture() : ui_(qApp), context_(services()) {
        feature_.provide(context_);
        feature_.install(context_);
    }
    fv::AppContext& context() { return context_; }
    fv::MemorySettingsStore& settings() { return settings_; }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.settings = &settings_;
        made.ui = &ui_;
        return made;
    }

    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::AppContext context_;
    fv::RecordFeature feature_;
};

} // namespace

TEST_CASE("RecordFeature registers the Quick Record screen", "[widget]") {
    fvtest::AppTheme theme;
    RecordFixture fixture;
    fv::ScreenRegistry& screens = fixture.context().screens();

    const auto specs = screens.screens();
    REQUIRE(specs.size() == 2);
    CHECK(specs[0].order == fv::order::tab::quick_record);
    CHECK(specs[0].title == QStringLiteral("Quick Record"));
    CHECK(specs[0].key == fv::RecordFeature::kQuickRecordScreen);
    CHECK(specs[0].group == fv::kTranscriptionGroup);
    CHECK(specs[1].order == fv::order::tab::upload_file);
    CHECK(specs[1].title == QStringLiteral("Upload File"));
    CHECK(specs[1].key == fv::RecordFeature::kUploadScreen);
    CHECK(specs[1].key == fv::screen_key::upload);
    CHECK(specs[1].group == fv::kTranscriptionGroup);

    QWidget parent;
    QWidget* screen = screens.ensure(fv::RecordFeature::kQuickRecordScreen, &parent);
    auto* tab = qobject_cast<fv::QuickRecordTab*>(screen);
    REQUIRE(tab != nullptr);
    // The controller reaches the same tab, and one compact controller.
    auto& widgets = fixture.context().service<fv::RecordWidgets>();
    CHECK(widgets.quickRecord() == tab);
    CHECK(screens.ensure(fv::RecordFeature::kQuickRecordScreen, &parent) == tab);
    CHECK(widgets.compactController() == widgets.compactController());

    auto* upload =
        qobject_cast<fv::UploadFileTab*>(screens.ensure(fv::RecordFeature::kUploadScreen, &parent));
    REQUIRE(upload != nullptr);
    CHECK(widgets.upload() == upload);
    CHECK(widgets.engineSelection().views().size() == 2);
    // The presenter is provided, and reaches the tab the window built.
    CHECK(fixture.context().service<fv::UploadPresenter>().tab() == upload);
}

TEST_CASE("the tab built by the feature follows the saved engine", "[widget]") {
    fvtest::AppTheme theme;
    RecordFixture fixture;
    auto& widgets = fixture.context().service<fv::RecordWidgets>();
    fv::QuickRecordTab* tab = widgets.quickRecord();
    REQUIRE(widgets.engineSelection().views().size() == 1);

    fixture.settings().update(fv::Json{{"local_asr_devices", {{"parakeet", "cpu"}}}});
    CHECK(tab->localEngine()->deviceCombo()->currentData().toString() == QStringLiteral("cpu"));
    delete tab;
    CHECK(widgets.engineSelection().views().empty());
    // A save after the tab is gone reaches no one and does not fail.
    fixture.settings().update(fv::Json{{"local_asr_devices", {{"parakeet", "auto"}}}});
}

TEST_CASE("widgets nothing took go with the feature's service, the others stay", "[widget]") {
    fvtest::AppTheme theme;
    fv::MemorySettingsStore settings;
    fv::qt::QtUiExecutor ui(qApp);
    QWidget window;
    QPointer<fv::QuickRecordTab> tab;
    QPointer<fv::CompactRecordController> compact;
    {
        fv::RecordWidgets widgets(settings, ui);
        tab = widgets.quickRecord(&window);
        compact = widgets.compactController();
    }
    CHECK(tab != nullptr);
    CHECK(compact == nullptr);
}

TEST_CASE("RecordFeature connects every signal", "[widget]") {
    fvtest::AppTheme theme;
    RecordFixture fixture;
    auto& widgets = fixture.context().service<fv::RecordWidgets>();
    fv::QuickRecordTab* tab = widgets.quickRecord();
    fv::CompactRecordController* compact = widgets.compactController();

    QWidget parent;
    auto* upload = qobject_cast<fv::UploadFileTab*>(
        fixture.context().screens().ensure(fv::RecordFeature::kUploadScreen, &parent));
    REQUIRE(upload != nullptr);

    checkConnected(*tab, &fv::QuickRecordTab::staticMetaObject, kUnusedTabSignals);
    checkConnected(*compact, &fv::CompactRecordController::staticMetaObject, kUnusedCompactSignals);
    checkConnected(*upload, &fv::UploadFileTab::staticMetaObject, kUnusedUploadSignals);
    delete tab;
    delete compact;
    delete upload;
}
