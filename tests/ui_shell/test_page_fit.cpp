// The main window fits itself to the open tab's page (main_window.py:1225-1420, 1915-1961),
// with stand-in pages of Quick Record's scroll-area structure. Ported from
// tests/test_window_fits_tab.py, plus the cases its lines define.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>

#include "appkit/order.h"
#include "appkit/registries.h"
#include "settings/keys.h"
#include "shell_fixture.h"
#include "stand_ins.h"
#include "ui_shell/main_window.h"
#include "ui_shell/page_fit.h"
#include "ui_widgets/tabbed_content.h"
#include "uimath/window_fit.h"
#include "uimath/window_geometry.h"

using fv::MainWindow;
using fv::TabbedContentWidget;
using fvtest::ShellFixture;
using fvtest::StandInPage;

namespace {

// Registers a stand-in page for the tab at `order`, built with the window (no key) or on first
// show (a key), and reports it once built.
using PageRef = std::shared_ptr<QPointer<StandInPage>>;

PageRef addPage(ShellFixture& shell, int order, std::vector<int> rows, std::string key = {},
                std::string group = {}) {
    auto made = std::make_shared<QPointer<StandInPage>>();
    fv::ScreenSpec spec{order, QStringLiteral("Stand-in"),
                        [made, rows](QWidget* parent) -> QWidget* {
                            *made = new StandInPage(rows, parent);
                            return made->data();
                        },
                        std::move(key), std::move(group)};
    shell.context().screens().addScreen(std::move(spec));
    return made;
}

// test_window_fits_tab.py's fixture: a 1920 x 1080 screen, shown, then settled.
std::unique_ptr<MainWindow> shownWindow(ShellFixture& shell) {
    auto window = shell.window();
    window->maxUsableHeight = [] {
        return 1080;
    };
    window->show();
    fvtest::settleWindow(*window);
    return window;
}

int chrome(const MainWindow& window) {
    return window.height() - window.tabbedContent()->stack()->height();
}

int contentHeight(const StandInPage& page) {
    return page.scrollArea()->widget()->layout()->sizeHint().height();
}

} // namespace

// from tests/test_window_fits_tab.py::test_showing_the_window_again_fits_it_at_once
TEST_CASE("showing the window again fits it at once", "[test_window_fits_tab][widget]") {
    ShellFixture shell;
    PageRef page = addPage(shell, fv::order::tab::quick_record, {200, 160});
    auto window = shownWindow(shell);
    const int fitted = window->height();
    REQUIRE(fitted != 900);
    CHECK((*page)->scrollArea()->verticalScrollBar()->maximum() == 0);

    window->hide();
    window->resize(window->width(), 900);
    window->show();
    QApplication::processEvents();

    CHECK(window->height() == fitted);
}

TEST_CASE("the window fits its height to the page and follows rows that come and go", "[widget]") {
    ShellFixture shell;
    PageRef page = addPage(shell, fv::order::tab::quick_record, {200, 160});
    auto window = shownWindow(shell);
    StandInPage& quick = **page;
    CHECK(window->height() == chrome(*window) + contentHeight(quick));
    // A short page lets the window go no shorter than the page itself.
    CHECK(window->minimumHeight() == std::min(window->height(), fv::window_size::kMinHeight));
    CHECK(quick.scrollArea()->verticalScrollBar()->maximum() == 0);

    const int before = window->height();
    QWidget* row = quick.addRow(240);
    fvtest::settleWindow(*window);
    CHECK(window->height() == before + 240 + 12);
    CHECK(quick.scrollArea()->verticalScrollBar()->maximum() == 0);
    CHECK(window->minimumHeight() == fv::window_size::kMinHeight);

    row->hide();
    fvtest::settleWindow(*window);
    CHECK(window->height() == before);
}

TEST_CASE("a page taller than the screen is capped at the screen's height", "[widget]") {
    ShellFixture shell;
    PageRef page = addPage(shell, fv::order::tab::quick_record, {400, 400, 400, 400});
    auto window = shownWindow(shell);
    CHECK(window->height() == 1080);
    CHECK(window->minimumHeight() == fv::window_size::kMinHeight);
    CHECK((*page)->scrollArea()->verticalScrollBar()->maximum() > 0);
}

TEST_CASE("a dragged height stays until the page changes, and an elastic page keeps it",
          "[widget]") {
    const bool elastic = GENERATE(false, true);
    ShellFixture shell;
    PageRef page = addPage(shell, fv::order::tab::quick_record, {200, 160});
    auto window = shownWindow(shell);
    window->setPageElastic(*page, elastic);
    CHECK(window->isPageElastic(*page) == elastic);
    fvtest::settleWindow(*window);
    const int fitted = window->height();

    const QPoint start = window->geometry().bottomLeft();
    window->beginResize(fv::ResizeEdge{0, 1}, start);
    window->applyResizeDelta(start + QPoint(0, 900 - window->height()));
    window->finishResize();
    fvtest::settleWindow(*window);
    CHECK(window->height() == 900);

    (*page)->addRow(40);
    fvtest::settleWindow(*window);
    if (elastic) {
        CHECK(window->height() == 900);
        CHECK(window->pageFit().quickRecordHeight() == 900);
    } else {
        CHECK(window->height() == fitted + 40 + 12);
    }
}

TEST_CASE("a page that needs more width widens the window and the next page gets it back",
          "[widget]") {
    ShellFixture shell;
    PageRef wide = addPage(shell, fv::order::tab::quick_record, {200});
    PageRef narrow = addPage(shell, fv::order::tab::walkthrough, {200});
    auto window = shell.window();
    window->maxUsableHeight = [] {
        return 1080;
    };
    (*wide)->scrollArea()->widget()->layout()->itemAt(0)->widget()->setMinimumWidth(900);
    window->show();
    fvtest::settleWindow(*window);
    const int own = fv::window_size::kDefaultWidth;
    CHECK(window->width() > own);
    CHECK(window->pageFit().ownWidth() == own);
    CHECK((*wide)->scrollArea()->contentsRect().width() >=
          (*wide)->scrollArea()->widget()->minimumSizeHint().width());

    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    fvtest::settleWindow(*window);
    CHECK(window->width() == own);
    CHECK_FALSE(window->pageFit().ownWidth().has_value());

    // The saved width is the user's own while a page has widened the window.
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    fvtest::settleWindow(*window);
    REQUIRE(window->width() > own);
    window->saveGeometryNow();
    CHECK(shell.settings().get(fv::keys::window_geometry)["width"] == own);
}

TEST_CASE("the page's scroll bar is held off while the window animates", "[widget]") {
    ShellFixture shell;
    PageRef page = addPage(shell, fv::order::tab::quick_record, {200});
    auto window = shownWindow(shell);
    QScrollArea* scroll = (*page)->scrollArea();
    CHECK(scroll->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded);

    window->pageFit().animateResize(window->width(), window->height() + 100);
    CHECK(window->pageFit().animationRunning());
    CHECK(scroll->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    fvtest::settleWindow(*window);
    CHECK_FALSE(window->pageFit().animationRunning());
    CHECK(scroll->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded);
}

TEST_CASE("a page without a scroll area keeps the size the user gave the window", "[widget]") {
    ShellFixture shell;
    auto window = shownWindow(shell);
    const QSize before = window->size();
    window->refitPage(false);
    CHECK(window->size() == before);
    CHECK(fv::pageScrollArea(window->page(TabbedContentWidget::TabQuickRecord)) == nullptr);
}

TEST_CASE("a collapsed transcript syncs the other transcription tabs", "[widget]") {
    ShellFixture shell;
    PageRef quick =
        addPage(shell, fv::order::tab::quick_record, {200}, "quick", fv::kTranscriptionGroup);
    PageRef other =
        addPage(shell, fv::order::tab::walkthrough, {200}, "other", fv::kTranscriptionGroup);
    auto window = shownWindow(shell);
    REQUIRE_FALSE(quick->isNull());
    QObject::connect(*quick, &StandInPage::transcriptionCollapsed, window.get(),
                     &MainWindow::onTranscriptionCollapsed);

    Q_EMIT(*quick)->transcriptionCollapsed(false, 120);
    CHECK_FALSE((*quick)->calls.contains(QStringLiteral("setTranscriptionCollapsed:0")));
    // A tab built later takes the state as it opens.
    REQUIRE(other->isNull());
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    REQUIRE_FALSE(other->isNull());
    CHECK((*other)->calls.contains(QStringLiteral("setTranscriptionCollapsed:0")));
}

// transcription_tab_base.py:997-999: fills_height() is the open transcript, asked at fit time.
TEST_CASE("an open transcript makes every transcription tab elastic", "[widget]") {
    ShellFixture shell;
    PageRef quick =
        addPage(shell, fv::order::tab::quick_record, {200}, "quick", fv::kTranscriptionGroup);
    // Upload File's place in the group is taken by a screen on the Walkthrough tab: the shell
    // registers the bench as the upload screen.
    PageRef other =
        addPage(shell, fv::order::tab::walkthrough, {200}, "other", fv::kTranscriptionGroup);
    auto window = shownWindow(shell);
    REQUIRE_FALSE(quick->isNull());
    // Collapsed on launch: nothing keeps a height.
    CHECK_FALSE(window->isPageElastic(*quick));
    QObject::connect(*quick, &StandInPage::transcriptionCollapsed, window.get(),
                     &MainWindow::onTranscriptionCollapsed);

    Q_EMIT(*quick)->transcriptionCollapsed(false, 0);
    fvtest::settleWindow(*window);
    CHECK(window->isPageElastic(*quick));
    // The other tab opens with its transcript open at the height an open transcript first gets.
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    fvtest::settleWindow(*window);
    REQUIRE_FALSE(other->isNull());
    CHECK(window->isPageElastic(*other));
    CHECK(window->height() == fv::window_size::kTranscriptionExpandHeight);
    // A tab outside the group is not a transcription tab.
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabUploadFile);
    CHECK_FALSE(window->isPageElastic(window->page(TabbedContentWidget::TabUploadFile)));

    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabQuickRecord);
    Q_EMIT(*quick)->transcriptionCollapsed(true, 0);
    CHECK_FALSE(window->isPageElastic(*quick));
    CHECK_FALSE(window->isPageElastic(*other));
}

TEST_CASE("quick record follows a transcript collapsed on another tab outside the group",
          "[widget]") {
    ShellFixture shell;
    // Quick Record built with the window, as a screen with no key.
    PageRef quick = addPage(shell, fv::order::tab::quick_record, {200});
    PageRef other =
        addPage(shell, fv::order::tab::walkthrough, {200}, "other", fv::kTranscriptionGroup);
    auto window = shownWindow(shell);
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    REQUIRE_FALSE(other->isNull());
    QObject::connect(*other, &StandInPage::transcriptionCollapsed, window.get(),
                     &MainWindow::onTranscriptionCollapsed);

    Q_EMIT(*other)->transcriptionCollapsed(false, 0);
    CHECK((*quick)->calls.contains(QStringLiteral("setTranscriptionCollapsed:0")));
    CHECK_FALSE((*other)->calls.contains(QStringLiteral("setTranscriptionCollapsed:0")));
    CHECK(window->isPageElastic(*quick));
}

TEST_CASE("a transcript toggled on another transcription tab moves the window by its height",
          "[widget]") {
    ShellFixture shell;
    PageRef other =
        addPage(shell, fv::order::tab::walkthrough, {300, 200}, "other", fv::kTranscriptionGroup);
    auto window = shownWindow(shell);
    window->tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
    fvtest::settleWindow(*window);
    const int open = window->height();

    window->onTranscriptionCollapsed(true, 150);
    fvtest::settleWindow(*window);
    CHECK(window->height() == open - 150);
    CHECK(window->minimumHeight() <= open - 150);
    CHECK(window->pageFit().transcriptHeight() == open);

    window->onTranscriptionCollapsed(false, 150);
    fvtest::settleWindow(*window);
    CHECK(window->height() == open);
}
