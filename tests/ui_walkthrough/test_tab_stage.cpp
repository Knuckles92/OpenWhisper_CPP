// The Walkthrough tab's stage (ui_qt/widgets/walkthrough_tab.py): the storyboard of the newest
// walkthrough, the recording card, the empty stage with recent walkthroughs, closing and reloading
// the walkthrough; and the window fitting the tab. Ported from tests/test_walkthrough_storyboard.py
// (the tab's ten tests and the eight window and stage width tests) and the Walkthrough half of
// tests/test_window_fits_tab.py.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QAbstractAnimation>
#include <QApplication>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
#include <QToolButton>

#include "settings/keys.h"
#include "tab_support.h"
#include "ui_walkthrough/walkthrough_ready.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/studio.h"
#include "window_support.h"

namespace fvtest {
namespace {

using fv::Json;
using fv::TabbedContentWidget;

TabRig::Options at(const std::filesystem::path& root) {
    TabRig::Options options;
    options.root = root;
    return options;
}

} // namespace

// ---- the Walkthrough tab ----

// from tests/test_walkthrough_storyboard.py::
//     test_the_tab_opens_on_the_newest_walkthrough_beside_its_settings
TEST_CASE("the tab opens on the newest walkthrough beside its settings",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    auto* tab = rig.tab.get();

    CHECK(rig->review()->folder() == capture.folder);
    CHECK(rig->stageState() == QStringLiteral("review"));
    CHECK(rig->fillsHeight());
    CHECK_FALSE(rig->review()->isHidden());
    CHECK(rig->emptyPane()->isHidden());
    CHECK(rig->recordingPane()->isHidden());
    // Nothing folds away: the settings stay open beside the storyboard.
    CHECK(rig->column()->isVisibleTo(tab));
    CHECK(rig->copyField()->isVisibleTo(tab));
    CHECK(rig->writeUpSwitch()->isVisibleTo(tab));
}

// from tests/test_walkthrough_storyboard.py::
//     test_with_no_walkthrough_the_stage_says_how_and_offers_none
TEST_CASE("with no walkthrough the stage says how and offers none",
          "[test_walkthrough_storyboard][widget]") {
    fv::testkit::TempDataRoot scratch;
    TabRig rig(at(scratch.path() / "none"));
    CHECK(rig->isRecentLoading());
    rig.settle();

    CHECK_FALSE(rig->isRecentLoading());
    CHECK(rig->review()->isHidden());
    CHECK(rig->stageState() == QStringLiteral("empty"));
    CHECK_FALSE(rig->emptyPane()->isHidden());
    CHECK(rig->recentRows().empty());
    CHECK(rig->recentBox()->isHidden());
    CHECK(rig->emptyHint()->text() == QStringLiteral("Start one on the left."));
    // The same size in every state, so the window never jumps.
    CHECK(rig->fillsHeight());
}

// from tests/test_walkthrough_storyboard.py::
//     test_closing_the_walkthrough_puts_the_tab_back_until_one_is_chosen
TEST_CASE("closing the walkthrough puts the tab back until one is chosen",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));

    CHECK(rig->review()->closeButton()->isVisibleTo(rig.tab.get()));
    rig->review()->closeButton()->click();
    rig.settle();
    CHECK(rig->review()->isHidden());
    CHECK(rig->stageState() == QStringLiteral("empty"));
    // It is still there to open again from the stage.
    REQUIRE(rig->recentRows().size() == 1);
    CHECK(rig->recentRows()[0]->folder() == capture.folder);
    // Still the newest, so the next launch opens without it too.
    fv::WalkthroughTabOptions options;
    options.capturesRoot = capture.root;
    options.io = &rig.io;
    {
        fv::WalkthroughTab relaunched(*rig.settings, {}, options);
        CHECK(relaunched.review()->isHidden());
        relaunched.close();
    }
    CHECK(rig->showWalkthrough(capture.folder));
    CHECK(rig->review()->folder() == capture.folder);
}

// from tests/test_walkthrough_storyboard.py::test_a_recent_walkthrough_opens_on_the_stage
TEST_CASE("a recent walkthrough opens on the stage", "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    rig->closeWalkthrough();
    rig.settle();
    REQUIRE(rig->recentRows().size() == 1);
    fv::WalkthroughRecentRow* row = rig->recentRows()[0];
    CHECK(row->titleLabel()->text() == QStringLiteral("first second last"));
    CHECK(row->accessibleName().contains(QStringLiteral("2 screenshots")));

    QTest::keyClick(row, Qt::Key_Return);

    CHECK(rig->stageState() == QStringLiteral("review"));
    CHECK(rig->review()->folder() == capture.folder);
}

// from tests/test_walkthrough_storyboard.py::
//     test_a_recording_takes_the_stage_and_gives_the_storyboard_back
TEST_CASE("a recording takes the stage and gives the storyboard back",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    rig->setRecordingState(true, true);
    CHECK(rig->stageState() == QStringLiteral("recording"));
    CHECK_FALSE(rig->recordingPane()->isHidden());
    CHECK(rig->review()->isHidden());

    // Cancelled: the walkthrough that was open is there again, edits and all.
    rig->setRecordingState(false, false);
    CHECK(rig->stageState() == QStringLiteral("review"));
    CHECK(rig->review()->folder() == capture.folder);
    CHECK_FALSE(rig->review()->isHidden());
}

// from tests/test_walkthrough_storyboard.py::test_a_dictation_leaves_the_stage_alone
TEST_CASE("a dictation leaves the stage alone", "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    rig->setRecordingState(true, false);
    CHECK(rig->stageState() == QStringLiteral("review"));
}

// from tests/test_walkthrough_storyboard.py::test_closing_asks_before_letting_unsaved_edits_go
TEST_CASE("closing asks before letting unsaved edits go", "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    rig->review()->rows()[0]->editor()->setPlainText(QStringLiteral("mine"));
    rig.reviewAnswer = QMessageBox::No;

    rig->review()->closeButton()->click();

    CHECK_FALSE(rig->review()->isHidden());
    CHECK(rig->review()->rows()[0]->editor()->toPlainText() == QStringLiteral("mine"));
}

// from tests/test_walkthrough_storyboard.py::test_only_the_tab_offers_to_close_its_walkthrough
TEST_CASE("only the tab offers to close its walkthrough", "[test_walkthrough_storyboard][widget]") {
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    fv::WalkthroughReview review(settings);
    CHECK(review.closeButton()->isHidden());
}

// from tests/test_walkthrough_storyboard.py::
//     test_a_finished_walkthrough_never_replaces_unsaved_edits
TEST_CASE("a finished walkthrough never replaces unsaved edits",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    rig->review()->rows()[0]->editor()->setPlainText(QStringLiteral("mine"));
    CHECK_FALSE(rig->showWalkthrough(capture.folder));
    CHECK(rig->review()->rows()[0]->editor()->toPlainText() == QStringLiteral("mine"));
}

// from tests/test_walkthrough_storyboard.py::
//     test_saving_in_the_tab_says_the_walkthrough_lists_are_stale
TEST_CASE("saving in the tab says the walkthrough lists are stale",
          "[test_walkthrough_storyboard][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    QSignalSpy changed(rig.tab.get(), &fv::WalkthroughTab::walkthroughsChanged);
    rig->review()->rows()[0]->editor()->setPlainText(QStringLiteral("fixed"));
    rig->review()->saveButton()->click();
    rig.wait();
    QApplication::processEvents();
    CHECK_FALSE(rig->review()->notice()->text().startsWith(QStringLiteral("Couldn't finish:")));
    CHECK(changed.count() == 1);
    CHECK(rig->review()->folder() == capture.folder);
    CHECK(rig->review()->rows()[0]->editor()->toPlainText() == QStringLiteral("fixed"));
}

// closeWalkthrough saves the newest's name, and showWalkthrough(nullopt) passes it over until a
// newer walkthrough finishes.
TEST_CASE("a closed walkthrough stays closed until a newer one finishes",
          "[walkthrough_tab][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    rig->closeWalkthrough();
    CHECK(rig.saved().at("walkthrough_closed") == Json("2026-10-21_14-32-05"));
    CHECK(rig->showWalkthrough());
    CHECK_FALSE(rig->review()->folder().has_value());

    std::filesystem::copy(capture.folder, capture.root / "2026-10-22_09-00-00",
                          std::filesystem::copy_options::recursive);
    CHECK(rig->showWalkthrough());
    CHECK(rig->review()->folder() == capture.root / "2026-10-22_09-00-00");
}

// reload_walkthrough: unsaved edits stay; a walkthrough deleted elsewhere gives way to the newest;
// the empty stage reads its list again.
TEST_CASE("reloading keeps edits, follows a deleted walkthrough and rereads the list",
          "[walkthrough_tab][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    const auto older = capture.root / "2026-10-20_09-00-00";
    std::filesystem::copy(capture.folder, older, std::filesystem::copy_options::recursive);

    rig->review()->rows()[0]->editor()->setPlainText(QStringLiteral("mine"));
    std::filesystem::remove_all(capture.folder);
    rig->reloadWalkthrough();
    CHECK(rig->review()->folder() == capture.folder);

    rig->review()->setDirty(false);
    rig->reloadWalkthrough();
    CHECK(rig->review()->folder() == older);

    rig->closeWalkthrough();
    rig.settle();
    CHECK_FALSE(rig->isRecentLoading());
    rig->reloadWalkthrough();
    CHECK(rig->isRecentLoading());
    rig.settle();
    REQUIRE(rig->recentRows().size() == 1);
    CHECK(rig->recentRows()[0]->folder() == older);
}

// Recent walkthroughs load on the way into the empty stage only, and a stale read is dropped.
TEST_CASE("the empty stage reads its list on the way in and drops a stale read",
          "[walkthrough_tab][widget]") {
    Capture capture;
    TabRig rig(at(capture.root));
    CHECK(rig.io.queued() == 1); // the agent scan, no list: the storyboard shows
    rig.settle();
    rig->closeWalkthrough();
    CHECK(rig.io.queued() == 1);
    rig->setRecordingState(true, false); // still empty: no second read
    CHECK(rig.io.queued() == 1);
    rig->reloadWalkthrough(); // a newer read
    CHECK(rig.io.queued() == 2);
    rig.io.run_one();
    QApplication::processEvents();
    CHECK(rig->isRecentLoading());
    CHECK(rig->recentRows().empty());
    rig.settle();
    CHECK_FALSE(rig->isRecentLoading());
    CHECK(rig->recentRows().size() == 1);
}

// ---- room ----

// from tests/test_walkthrough_storyboard.py::
//     test_a_storyboard_widens_the_window_and_the_next_tab_gives_the_width_back
TEST_CASE("a storyboard widens the window and the next tab gives the width back",
          "[test_walkthrough_storyboard][widget]") {
    LargeCapture capture;
    WindowRig rig(capture.root);
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    REQUIRE(rig.tab);
    CHECK(rig.tab->scrollArea()->contentsRect().width() == rig.tab->preferredWidth());
    CHECK_FALSE(rig.tab->split()->isStacked());
    // The width the window keeps for next time is the user's own.
    rig.window->saveGeometryNow();
    CHECK(rig.settings().get(fv::keys::window_geometry)["width"] == fv::window_size::kDefaultWidth);

    rig.openTab(TabbedContentWidget::TabQuickRecord);
    CHECK(rig.window->width() == fv::window_size::kDefaultWidth);
}

// from
// tests/test_walkthrough_storyboard.py::test_opening_a_studio_tab_resizes_the_window_in_one_move
// (the Walkthrough half: the Meeting Mode tab is fv_ui_meeting's; a move is a start of the
// window's resize animation, whose end is where it goes)
TEST_CASE("opening a studio tab resizes the window in one move",
          "[test_walkthrough_storyboard][widget]") {
    LargeCapture capture;
    WindowRig rig(capture.root);
    std::vector<QSize> moves;
    // The window makes its animation on the first resize it animates: the tab's first opening
    // and the way back.
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    rig.openTab(TabbedContentWidget::TabQuickRecord);
    QPropertyAnimation* animation = rig.window->pageFit().animation();
    REQUIRE(animation != nullptr);
    QObject::connect(animation, &QAbstractAnimation::stateChanged, animation,
                     [&moves, animation](QAbstractAnimation::State state) {
                         if (state == QAbstractAnimation::Running) {
                             moves.push_back(animation->endValue().toRect().size());
                         }
                     });
    rig.openTab(TabbedContentWidget::TabQuickRecord);
    moves.clear();
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    CHECK(moves == std::vector<QSize>{rig.window->size()});
}

// from
// tests/test_walkthrough_storyboard.py::test_the_window_keeps_one_size_whatever_the_stage_shows
TEST_CASE("the window keeps one size whatever the stage shows",
          "[test_walkthrough_storyboard][widget]") {
    LargeCapture capture;
    WindowRig rig(capture.root);
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    REQUIRE(rig.tab);
    const QSize size = rig.window->size();
    const QRect column = rig.tab->column()->geometry();

    rig.tab->closeWalkthrough();
    rig.settle();
    CHECK(rig.tab->stageState() == QStringLiteral("empty"));
    CHECK(rig.window->size() == size);

    rig.tab->setRecordingState(true, true);
    rig.settle();
    CHECK(rig.window->size() == size);

    rig.tab->setRecordingState(false, false);
    rig.tab->showWalkthrough(capture.folder);
    rig.tab->writeUpSwitch()->click();
    rig.settle();
    CHECK(rig.tab->stageState() == QStringLiteral("review"));
    CHECK(rig.window->size() == size);
    CHECK(rig.tab->column()->geometry() == column);
}

// from tests/test_walkthrough_storyboard.py::test_a_window_already_wide_enough_keeps_its_width
TEST_CASE("a window already wide enough keeps its width", "[test_walkthrough_storyboard][widget]") {
    LargeCapture capture;
    WindowRig rig(capture.root);
    rig.window->setGeometry(40, 40, 1100, 700);
    rig.settle();
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    CHECK(rig.window->width() == 1100);
    rig.openTab(TabbedContentWidget::TabQuickRecord);
    CHECK(rig.window->width() == 1100);
}

// from tests/test_walkthrough_storyboard.py::
//     test_the_side_panel_adds_to_the_window_and_leaves_the_page_its_width
TEST_CASE("the side panel adds to the window and leaves the page its width",
          "[test_walkthrough_storyboard][widget]") {
    LargeCapture capture;
    WindowRig rig(capture.root);
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    REQUIRE(rig.tab);
    const int width = rig.window->width();
    const int page = rig.tab->scrollArea()->contentsRect().width();

    rig.togglePanel();
    CHECK(rig.window->width() == width + StandInSidePanel::kExpandedWidth);
    CHECK(rig.tab->scrollArea()->contentsRect().width() == page);
    CHECK_FALSE(rig.tab->split()->isStacked());

    rig.togglePanel();
    CHECK(rig.window->width() == width);
    CHECK(rig.tab->scrollArea()->contentsRect().width() == page);
}

// from tests/test_walkthrough_storyboard.py::
//     test_closing_the_side_panel_gives_back_what_a_small_screen_took
TEST_CASE("closing the side panel gives back what a small screen took",
          "[test_walkthrough_storyboard][widget]") {
    LargeCapture capture;
    WindowRig rig(capture.root);
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    REQUIRE(rig.tab);
    const int width = rig.window->width();
    rig.screenOverride = QRect(0, 0, width + 100, 1000);

    rig.togglePanel();
    CHECK(rig.window->width() == width + 100);

    rig.togglePanel();
    CHECK(rig.window->width() == width);
    CHECK_FALSE(rig.tab->split()->isStacked());
}

// from
// tests/test_walkthrough_storyboard.py::test_dragged_to_its_narrowest_the_storyboard_shows_whole
TEST_CASE("dragged to its narrowest the storyboard shows whole",
          "[test_walkthrough_storyboard][widget]") {
    // Nothing runs past the right edge: Open drops below Copy, screenshots shrink.
    LargeCapture capture;
    WindowRig rig(capture.root);
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    REQUIRE(rig.tab);
    const QRect geo = rig.window->geometry();
    rig.window->setGeometry(geo.x(), geo.y(), fv::window_size::kMinWidth, geo.height());
    rig.settle();

    CHECK(rig.window->width() == fv::window_size::kMinWidth);
    QWidget* viewport = rig.tab->scrollArea()->viewport();
    CHECK(rig.tab->scrollArea()->widget()->width() == viewport->width());
    fv::WalkthroughReview* review = rig.tab->review();
    CHECK(review->actionRow()->isStacked());
    const int edge = review->mapTo(viewport, review->rect().topRight()).x();
    CHECK(edge < viewport->width());
    for (fv::ReviewRow* row : review->rows()) {
        CHECK(row->width() <= review->board()->width());
    }
}

// from tests/test_walkthrough_storyboard.py::test_rows_fit_the_stage_at_its_narrowest
TEST_CASE("rows fit the stage at its narrowest", "[test_walkthrough_storyboard][widget]") {
    // Beside the side panel the stage can be this narrow: the switch drops under the step.
    LargeCapture capture;
    fv::SettingsStore settings(std::make_unique<fv::MemorySettingsFile>());
    fv::WalkthroughReview widget(settings);
    widget.showFolder(capture.folder);
    widget.show();
    std::vector<bool> stacked;
    for (int width : {fv::studio::kStageMin.width(), 900}) {
        CAPTURE(width);
        widget.resize(width, 700);
        for (int pass = 0; pass < 3; ++pass) {
            QApplication::processEvents();
        }
        CHECK(widget.board()->width() == widget.scroll()->viewport()->width());
        for (fv::ReviewRow* row : widget.rows()) {
            CHECK(row->geometry().right() < widget.board()->width());
            if (row->check()->isVisible()) {
                const int edge = row->check()->mapTo(row, row->check()->rect().topRight()).x();
                CHECK(edge < row->width());
            }
        }
        auto* top = qobject_cast<fv::Reflow*>(widget.rows()[0]->top());
        REQUIRE(top != nullptr);
        stacked.push_back(top->isStacked());
        // The copy buttons wrap rather than squash into each other.
        for (QWidget* button : std::vector<QWidget*>{widget.pdfButton(), widget.markdownButton(),
                                                     widget.copyWordButton(), widget.chip()}) {
            CHECK(button->width() >= button->sizeHint().width());
            CHECK(button->mapTo(&widget, button->rect().topRight()).x() < widget.width());
        }
    }
    CHECK(stacked == std::vector<bool>{true, false});
    widget.close();
}

// ---- tests/test_window_fits_tab.py ----

namespace {

WindowRig::Options fitsTab() {
    WindowRig::Options options;
    options.screen = QRect(0, 0, 1920, 1080);
    options.maxUsableHeight = 1080;
    options.geometry.reset();
    options.sidePanel = false;
    return options;
}

} // namespace

// from
// tests/test_window_fits_tab.py::test_the_walkthrough_tab_keeps_its_size_as_its_settings_change
TEST_CASE("the walkthrough tab keeps its size as its settings change",
          "[test_window_fits_tab][widget]") {
    fv::testkit::TempDataRoot scratch;
    WindowRig rig(scratch.path() / "captures", fitsTab());
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    REQUIRE(rig.tab);
    rig.tab->writeUpSwitch()->setChecked(false);
    rig.settle();
    const QSize size = rig.window->size();

    rig.tab->writeUpSwitch()->setChecked(true);
    rig.tab->autoSwitch()->setChecked(true);
    rig.settle();
    CHECK(rig.window->size() == size);
    CHECK(rig.tab->scrollArea()->verticalScrollBar()->maximum() == 0);

    rig.tab->writeUpSwitch()->setChecked(false);
    rig.settle();
    CHECK(rig.window->size() == size);
}

// from tests/test_window_fits_tab.py::test_a_dragged_walkthrough_height_stays
TEST_CASE("a dragged walkthrough height stays", "[test_window_fits_tab][widget]") {
    // Its stage takes the height it is given, as an open transcript does.
    fv::testkit::TempDataRoot scratch;
    WindowRig rig(scratch.path() / "captures", fitsTab());
    rig.openTab(TabbedContentWidget::TabWalkthrough);
    REQUIRE(rig.tab);
    const int fitted = rig.window->height();

    rig.window->resize(rig.window->width(), fitted + 120);
    rig.settle();
    rig.tab->writeUpSwitch()->setChecked(!rig.tab->writeUpSwitch()->isChecked());
    rig.settle();
    CHECK(rig.window->height() == fitted + 120);
    CHECK(rig.tab->stage()->height() > 0);
}

} // namespace fvtest
