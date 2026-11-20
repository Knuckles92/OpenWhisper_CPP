// Quick Record and the compact controller in M1.21's main window, which knows them only by Qt
// method name (lead amendment 21): the status line, the recording-state relay and the transcript
// collapse sync reach them through their slots, and Quick Record is a keyed screen of the
// transcription group.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

#include <QApplication>
#include <QLabel>
#include <QMetaObject>
#include <QRect>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_record/compact_record_controller.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/downloads_label.h"

namespace {

// What Bootstrap builds for the shell and the record feature: a data folder of its own,
// in-memory settings, the GUI executor, every feature provided, then installed, in the
// registry's order (by id: "record" before "shell"), then the window completed.
class WindowFixture {
public:
    WindowFixture()
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), ui_(qApp),
          context_(services()) {
        record_.provide(context_);
        shell_.provide(context_);
        record_.install(context_);
        shell_.install(context_);
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        window().completeSetup();
    }

    fv::MainWindow& window() { return context_.service<fv::MainWindow>(); }
    fv::RecordWidgets& widgets() { return context_.service<fv::RecordWidgets>(); }
    fv::AppContext& context() { return context_; }
    // The tab the window built for its Quick Record page.
    fv::QuickRecordTab* quickRecord() {
        return qobject_cast<fv::QuickRecordTab*>(
            context_.screens().existing(fv::RecordFeature::kQuickRecordScreen));
    }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    fv::RecordFeature record_;
};

bool declares(const QObject& object, const char* signature) {
    return object.metaObject()->indexOfMethod(QMetaObject::normalizedSignature(signature)) >= 0;
}

} // namespace

TEST_CASE("the widgets declare what the main window calls by name", "[widget]") {
    WindowFixture fixture;
    fv::QuickRecordTab* tab = fixture.widgets().quickRecord();
    for (const char* signature :
         {"setStatus(QString)", "setRecordingActive(bool)", "setTranscriptionCollapsed(bool)"}) {
        INFO(signature);
        CHECK(declares(*tab, signature));
    }
    fv::CompactRecordController* compact = fixture.widgets().compactController();
    for (const char* signature : {"setStatus(QString)", "setRecordingState(bool)"}) {
        INFO(signature);
        CHECK(declares(*compact, signature));
    }
    delete compact;
}

TEST_CASE("Quick Record is the window's first tab and a keyed transcription screen", "[widget]") {
    WindowFixture fixture;
    fv::QuickRecordTab* tab = fixture.quickRecord();
    REQUIRE(tab != nullptr);
    CHECK(fixture.window().page(0) == tab);
    CHECK(fixture.widgets().quickRecord() == tab);
    const std::vector<QWidget*> group =
        fixture.context().screens().existingInGroup(fv::kTranscriptionGroup);
    CHECK(std::ranges::find(group, tab) != group.end());
}

TEST_CASE("the window's status reaches Quick Record and the compact controller", "[widget]") {
    WindowFixture fixture;
    fv::QuickRecordTab* tab = fixture.quickRecord();
    REQUIRE(tab != nullptr);
    fv::CompactRecordController* compact = fixture.widgets().compactController();
    fixture.window().setCompactController(compact);

    fixture.window().setStatus(QStringLiteral("Copied to clipboard"));
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Copied to clipboard"));
    CHECK(compact->statusLabel()->text() == QStringLiteral("Copied to clipboard"));
    // "Ready to record" is the idle readout on the tab, and a line of its own on the controller.
    fixture.window().setStatus(QStringLiteral("Ready to record"));
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Ready to record"));
    CHECK(compact->statusLabel()->text() == QStringLiteral("Ready to record"));
}

TEST_CASE("the window's recording state reaches Quick Record and the compact controller",
          "[widget]") {
    WindowFixture fixture;
    fv::QuickRecordTab* tab = fixture.quickRecord();
    REQUIRE(tab != nullptr);
    fv::CompactRecordController* compact = fixture.widgets().compactController();
    fixture.window().setCompactController(compact);

    fixture.window().setRecordingActive(true);
    CHECK(tab->isRecording());
    CHECK(tab->recordButton()->text() == QStringLiteral("Recording..."));
    CHECK(tab->stopButton()->isActive());
    CHECK(tab->resolvedLabel()->text() == QStringLiteral("Recording in progress..."));
    CHECK(compact->recordButton()->text() == QStringLiteral("Recording"));
    CHECK(compact->stopButton()->isActive());
    CHECK(compact->statusLabel()->text() == QStringLiteral("Recording in progress..."));

    fixture.window().setRecordingActive(false);
    CHECK_FALSE(tab->isRecording());
    CHECK(tab->recordButton()->text() == QStringLiteral("Start Recording"));
    CHECK(compact->recordButton()->text() == QStringLiteral("Record"));
    CHECK_FALSE(compact->stopButton()->isActive());
    CHECK(compact->statusLabel()->text() == QStringLiteral("Ready to record"));
}

TEST_CASE("a transcript opened on another tab opens Quick Record's without a signal", "[widget]") {
    WindowFixture fixture;
    fv::QuickRecordTab* tab = fixture.quickRecord();
    REQUIRE(tab != nullptr);
    REQUIRE(tab->isTranscriptionCollapsed());
    int emitted = 0;
    QObject::connect(tab, &fv::QuickRecordTab::transcriptionCollapsed, tab,
                     [&emitted] { ++emitted; });

    // No sender: as if the Upload tab's card had been toggled.
    fixture.window().onTranscriptionCollapsed(false, 120);
    CHECK_FALSE(tab->isTranscriptionCollapsed());
    fixture.window().onTranscriptionCollapsed(true, 120);
    CHECK(tab->isTranscriptionCollapsed());
    CHECK(emitted == 0);
}
