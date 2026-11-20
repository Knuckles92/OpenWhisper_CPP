// A result's stats card scrolled into view below the transcript when the window cannot grow
// for it (MainWindow._on_stats_visibility_changed and _reveal_shown_stats). The tab's page is a
// stand-in with the shape of a transcription tab: a scroll area whose content is taller than its
// viewport, the stats card at the bottom.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QRect>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include "shell_fixture.h"
#include "ui_shell/main_window.h"

using fvtest::ShellFixture;

namespace {

// A page as a transcription tab lays it out: everything in one scroll area, the stats last.
struct StandInPage {
    QScrollArea area;
    QWidget* content = new QWidget;
    QLabel* stats = nullptr;

    StandInPage() {
        auto* layout = new QVBoxLayout(content);
        auto* transcript = new QWidget;
        transcript->setMinimumHeight(600);
        layout->addWidget(transcript);
        stats = new QLabel(QStringLiteral("1.2 s · 3.4 s of audio · 56 KB"));
        stats->setFixedHeight(40);
        stats->hide();
        layout->addWidget(stats);
        area.setWidgetResizable(true);
        area.setWidget(content);
        area.resize(680, 400);
    }
};

void waitMs(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

} // namespace

// from
// tests/test_main_window_compact.py::TestMainWindowCompactMode::test_result_stats_scroll_into_view_when_the_window_cannot_grow
TEST_CASE("test_result_stats_scroll_into_view_when_the_window_cannot_grow",
          "[test_main_window_compact][widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->availableScreenRect = [] {
        return QRect(0, 0, 1024, 640);
    };
    window->setGeometry(0, 0, 680, 640);
    window->show();
    StandInPage page;
    page.area.show();
    QApplication::processEvents();
    REQUIRE(page.area.verticalScrollBar()->value() == 0);

    page.stats->show();
    window->onStatsVisibilityChanged(true, page.stats);
    // Nothing moves until the card's section has finished opening.
    waitMs(fv::MainWindow::kStatsRevealDelayMs / 2);
    CHECK(page.area.verticalScrollBar()->value() == 0);
    for (int i = 0; i < 150 && page.area.verticalScrollBar()->value() == 0; ++i) {
        waitMs(10);
    }

    QWidget* viewport = page.area.viewport();
    const int top = page.stats->mapTo(viewport, page.stats->rect().topLeft()).y();
    CHECK(top >= 0);
    CHECK(top + page.stats->height() <= viewport->height());
}

TEST_CASE("a card hidden before the reveal is not scrolled to", "[widget]") {
    ShellFixture shell;
    auto window = shell.window();
    window->show();
    StandInPage page;
    page.area.show();
    QApplication::processEvents();

    // Shown, then hidden again before the reveal: the card is not looked for.
    page.stats->show();
    window->onStatsVisibilityChanged(true, page.stats);
    page.stats->hide();
    waitMs(fv::MainWindow::kStatsRevealDelayMs + 150);
    CHECK(page.area.verticalScrollBar()->value() == 0);

    // A card that hides says so; nothing is scheduled for it.
    window->onStatsVisibilityChanged(false, page.stats);
    waitMs(fv::MainWindow::kStatsRevealDelayMs + 150);
    CHECK(page.area.verticalScrollBar()->value() == 0);
}
