// The main window with the real Quick Record tab and compact controller plugged in by the
// controller (tests/test_main_window_compact.py, tests/test_window_fits_tab.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QPoint>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextEdit>

#include "controller_support.h"
#include "ui_record/compact_record_controller.h"
#include "ui_record/transcription_tab_base.h"
#include "ui_shell/page_fit.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/cards.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/wrapped_label.h"
#include "uimath/window_geometry.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;

namespace {

// _settle: two window animations and the transcript card's own can run back to back.
void settle(fv::MainWindow& window) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 800) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    waitUntil([&window] {
        return !window.pageFit().scheduled() && !window.pageFit().animationRunning();
    });
    QApplication::processEvents();
}

// _blank_below: empty height between the page's lowest visible control and its bottom.
int blankBelow(fv::TranscriptionTabBase& page) {
    QWidget* viewport = page.scrollArea()->viewport();
    int lowest = 0;
    for (QWidget* child : page.scrollArea()->widget()->findChildren<QWidget*>()) {
        if (child->isVisible() && child->findChildren<QWidget*>().isEmpty()) {
            lowest = std::max(lowest, child->mapTo(viewport, QPoint(0, child->height())).y());
        }
    }
    return viewport->height() - lowest;
}

fv::QuickRecordTab& openQuickRecord(ControllerRig& rig) {
    rig.window().show();
    rig.window().tabbedContent()->setCurrentIndex(fv::TabbedContentWidget::TabQuickRecord);
    settle(rig.window());
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    return *tab;
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompact::test_compact_controls_delegate_to_quick_record
TEST_CASE("test_compact_controls_delegate_to_quick_record", "[test_main_window_compact][widget]") {
    ControllerRig rig;
    fv::QuickRecordTab& tab = openQuickRecord(rig);
    std::vector<bool> toggles;
    int canceled = 0;
    QObject::connect(&tab, &fv::QuickRecordTab::recordToggled,
                     [&toggles](bool recording) { toggles.push_back(recording); });
    QObject::connect(&tab, &fv::QuickRecordTab::recordCanceled, [&canceled] { ++canceled; });
    rig.window().setCompactMode(true);
    fv::CompactRecordController* compact = rig.ui().compactController();
    REQUIRE(rig.window().compactController() == compact);

    compact->recordButton()->click();
    CHECK(toggles == std::vector<bool>{true});
    // The engine is not loaded, so the start was refused and the window stays idle.
    CHECK_FALSE(rig.window().isRecording());

    compact->cancelButton()->setEnabled(true);
    tab.cancelButton()->setEnabled(true);
    compact->cancelButton()->click();
    CHECK(canceled == 1);
    CHECK_FALSE(rig.window().isRecording());
}

// Compact Mode restored over another saved tab: Quick Record was never shown, and the compact
// Record button still records through it.
TEST_CASE("the compact controls record when Quick Record was never opened", "[widget]") {
    fvtest::RigOptions options;
    options.settings["last_tab_index"] = 1;
    ControllerRig rig(options);
    REQUIRE(rig.startEngine());
    REQUIRE(rig.window().tabbedContent()->currentIndex() == fv::TabbedContentWidget::TabUploadFile);
    REQUIRE(rig.quickRecord() == nullptr);
    rig.window().setCompactMode(true);

    rig.ui().compactController()->recordButton()->click();

    REQUIRE(rig.quickRecord() != nullptr);
    CHECK(waitUntil([&rig] { return rig.runtime().is_recording(); }));
    CHECK(rig.runtime().current_job() != nullptr);
    rig.ui().cancelRecording();
}

// from
// tests/test_main_window_compact.py::TestMainWindowCompact::test_quick_record_controls_fit_at_minimum_height
// (the profile card arrives with cleanup profiles, M4)
TEST_CASE("test_quick_record_controls_fit_at_minimum_height",
          "[test_main_window_compact][widget]") {
    ControllerRig rig;
    fv::QuickRecordTab& tab = openQuickRecord(rig);
    // Tallest resting layout: the Command Mode link row sits under the buttons.
    tab.cleanupCheck()->setChecked(true);
    tab.commandLink()->setVisible(true);
    tab.commandHint()->setVisible(false);

    rig.window().resize(fv::window_size::kDefaultWidth, fv::window_size::kMinHeight);
    QApplication::processEvents();

    CHECK(tab.scrollArea()->verticalScrollBar()->maximum() == 0);
    QWidget* viewport = tab.scrollArea()->viewport();
    for (QPushButton* button :
         std::vector<QPushButton*>{tab.recordButton(), tab.stopButton(), tab.cancelButton()}) {
        const int bottom = button->mapTo(viewport, button->rect().bottomLeft()).y();
        CHECK(bottom >= 0);
        CHECK(bottom < viewport->height());
    }
}

// The tab at `index`, opened and settled.
fv::TranscriptionTabBase& openTab(ControllerRig& rig, int index) {
    rig.window().show();
    rig.window().tabbedContent()->setCurrentIndex(index);
    settle(rig.window());
    auto* page = qobject_cast<fv::TranscriptionTabBase*>(
        rig.window().tabbedContent()->stack()->widget(index));
    REQUIRE(page != nullptr);
    return *page;
}

} // namespace

// from tests/test_window_fits_tab.py::test_every_tab_fits_the_window_to_its_page
// (Quick Record and Upload File; M6 and M7 add their tabs)
TEST_CASE("test_every_tab_fits_the_window_to_its_page", "[test_window_fits_tab][widget]") {
    const int tab =
        GENERATE(fv::TabbedContentWidget::TabQuickRecord, fv::TabbedContentWidget::TabUploadFile);
    ControllerRig rig;
    fv::TranscriptionTabBase& page = openTab(rig, tab);

    CHECK(page.scrollArea()->verticalScrollBar()->maximum() == 0);
    // Only the card padding and page margin are left below the last control.
    CHECK(blankBelow(page) < 48);
    // A short page lets the window go no shorter than the page itself.
    CHECK(rig.window().minimumHeight() ==
          std::min(rig.window().height(), fv::window_size::kMinHeight));
}

// from tests/test_window_fits_tab.py::test_an_open_transcript_keeps_the_height_the_user_gives_it
// (Quick Record's half; the Upload File half runs once M2's Upload tab exists)
TEST_CASE("test_an_open_transcript_keeps_the_height_the_user_gives_it",
          "[test_window_fits_tab][widget]") {
    ControllerRig rig;
    fv::QuickRecordTab& page = openQuickRecord(rig);
    fv::MainWindow& window = rig.window();
    const int collapsed = window.height();

    page.expandTranscription();
    settle(window);
    CHECK(window.height() > collapsed);
    const QPoint start = window.geometry().bottomLeft();
    window.beginResize(fv::ResizeEdge{0, 1}, start);
    window.applyResizeDelta(start + QPoint(0, 900 - window.height()));
    window.finishResize();
    settle(window);
    CHECK(window.height() == 900);

    page.transcriptionCard()->setCollapsed(true);
    settle(window);
    CHECK(window.height() == collapsed);
    CHECK(blankBelow(page) < 48);

    page.expandTranscription();
    settle(window);
    CHECK(window.height() == 900);
}

// from
// tests/test_window_fits_tab.py::test_quick_record_fits_wrapped_text_and_shrinks_after_a_long_result
TEST_CASE("test_quick_record_fits_wrapped_text_and_shrinks_after_a_long_result",
          "[test_window_fits_tab][widget]") {
    ControllerRig rig;
    fv::ui::ThemeManager manager(QStringLiteral("dark"));
    fv::ui::applyUiFontScale(100, *qApp, manager);
    fv::QuickRecordTab& page = openQuickRecord(rig);
    fv::MainWindow& window = rig.window();
    QTextEdit* editor = page.transcriptText();
    const QString shortText = QStringLiteral("A short recording.");
    page.setTranscript(shortText);
    page.expandTranscription();
    page.setTranscriptionStats(0.3, 3, 100'000);
    settle(window);
    const int shortHeight = window.height();
    const int shortEditorHeight = editor->height();

    CHECK(editor->verticalScrollBar()->maximum() == 0);
    CHECK(page.scrollArea()->verticalScrollBar()->maximum() == 0);

    page.setTranscript(QStringLiteral("A longer result with enough text to wrap. ").repeated(11));
    settle(window);
    CHECK(editor->height() > shortEditorHeight);
    CHECK(editor->verticalScrollBar()->maximum() == 0);

    page.setTranscript(QStringLiteral("A much longer transcript. ").repeated(250));
    settle(window);
    CHECK(window.height() > shortHeight);
    CHECK(editor->verticalScrollBar()->maximum() > 0);
    CHECK(page.scrollArea()->verticalScrollBar()->maximum() == 0);

    page.setTranscript(shortText);
    settle(window);
    CHECK(window.height() == shortHeight);
    CHECK(editor->height() == shortEditorHeight);
    CHECK(editor->verticalScrollBar()->maximum() == 0);
    qApp->setStyleSheet(QString());
}
