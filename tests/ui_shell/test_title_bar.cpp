// The custom title bar: its height follows the menu bar at every font scale, the window buttons
// paint their glyphs, maximize toggles with restore, and its stylesheet parses. Ported from
// tests/test_title_bar.py and tests/test_window_fit_and_title_bar.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QHBoxLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "shell_fixture.h"
#include "ui_shell/title_bar.h"
#include "ui_theme/font_scale.h"

using fv::TitleBar;

namespace {

struct Hosted {
    std::unique_ptr<QWidget> host;
    TitleBar* bar = nullptr;
};

// test_title_bar.py's fixture: a bar inside a 680 x 100 host, with File, View and Help menus.
Hosted hostedBar() {
    Hosted made;
    made.host = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(made.host.get());
    layout->setContentsMargins(0, 0, 0, 0);
    made.bar = new TitleBar(made.host.get());
    layout->addWidget(made.bar);
    for (const char* name : {"File", "View", "Help"}) {
        made.bar->menuBar()->addMenu(QString::fromLatin1(name))->addAction(QStringLiteral("Item"));
    }
    made.host->resize(680, 100);
    made.host->show();
    QApplication::processEvents();
    return made;
}

bool menuItemsVisible(const TitleBar& bar) {
    QMenuBar* menuBar = bar.menuBar();
    auto* overflow = menuBar->findChild<QToolButton*>(QStringLiteral("qt_menubar_ext_button"));
    if (overflow != nullptr && overflow->isVisible()) {
        return false;
    }
    for (QAction* action : menuBar->actions()) {
        if (!menuBar->rect().contains(menuBar->actionGeometry(action))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> capturedMessages;

void captureMessages(QtMsgType, const QMessageLogContext&, const QString& message) {
    capturedMessages.push_back(message.toStdString());
}

} // namespace

// from tests/test_title_bar.py::test_menu_items_fit_at_every_font_scale
TEST_CASE("menu items fit at every font scale", "[test_title_bar][widget]") {
    const int percent = GENERATE(90, 100, 115, 130);
    // Themed, as the app runs: the later scale changes rescale theme.qss too.
    fvtest::AppTheme theme;
    Hosted hosted = hostedBar();
    fv::ui::applyUiFontScale(percent, *qApp);
    QApplication::processEvents();
    QApplication::processEvents();

    CHECK(menuItemsVisible(*hosted.bar));
    CHECK(hosted.bar->height() >= TitleBar::kBaseHeight);
    CHECK(hosted.bar->height() >= hosted.bar->menuBar()->sizeHint().height());
    CHECK(hosted.bar->closeButton()->height() == hosted.bar->height());
}

// from tests/test_title_bar.py::test_default_scale_keeps_menus_and_a_usable_bar
TEST_CASE("default scale keeps menus and a usable bar", "[test_title_bar][widget]") {
    fvtest::AppTheme theme;
    Hosted hosted = hostedBar();
    fv::ui::applyUiFontScale(100, *qApp);
    QApplication::processEvents();

    CHECK(hosted.bar->height() >= TitleBar::kBaseHeight);
    CHECK(menuItemsVisible(*hosted.bar));
}

// from tests/test_title_bar.py::test_larger_scale_does_not_shrink_the_bar
TEST_CASE("larger scale does not shrink the bar", "[test_title_bar][widget]") {
    fvtest::AppTheme theme;
    Hosted hosted = hostedBar();
    fv::ui::applyUiFontScale(100, *qApp);
    QApplication::processEvents();
    const int defaultHeight = hosted.bar->height();

    fv::ui::applyUiFontScale(130, *qApp);
    QApplication::processEvents();
    QApplication::processEvents();

    CHECK(hosted.bar->height() >= defaultHeight);
    CHECK(menuItemsVisible(*hosted.bar));
}

// from tests/test_title_bar.py::test_window_buttons_paint_their_glyphs_not_font_characters
TEST_CASE("window buttons paint their glyphs not font characters", "[test_title_bar][widget]") {
    fvtest::AppTheme theme;
    Hosted hosted = hostedBar();
    const std::vector<fv::WindowGlyphButton*> buttons{
        hosted.bar->minimizeButton(), hosted.bar->maximizeButton(), hosted.bar->closeButton()};
    QStringList texts;
    QStringList glyphs;
    for (fv::WindowGlyphButton* button : buttons) {
        texts << button->text();
        glyphs << button->glyphName();
        CHECK_FALSE(button->grab().isNull());
    }
    CHECK(texts == QStringList{QString(), QString(), QString()});
    CHECK(glyphs == QStringList{QStringLiteral("minimize"), QStringLiteral("maximize"),
                                QStringLiteral("close")});
}

// from tests/test_title_bar.py::test_maximize_turns_into_restore_and_back
TEST_CASE("maximize turns into restore and back", "[test_title_bar][widget]") {
    fvtest::AppTheme theme;
    Hosted hosted = hostedBar();
    hosted.bar->toggleMaximize();
    CHECK(hosted.bar->maximizeButton()->glyphName() == QStringLiteral("restore"));
    CHECK(hosted.bar->maximizeButton()->toolTip() == QStringLiteral("Restore"));
    CHECK(hosted.bar->maximizeButton()->accessibleName() == QStringLiteral("Restore window"));
    hosted.bar->toggleMaximize();
    CHECK(hosted.bar->maximizeButton()->glyphName() == QStringLiteral("maximize"));
    CHECK(hosted.bar->maximizeButton()->toolTip() == QStringLiteral("Maximize"));
    CHECK(hosted.bar->maximizeButton()->accessibleName() == QStringLiteral("Maximize window"));
}

// from tests/test_window_fit_and_title_bar.py::test_title_bar_stylesheet_parses
TEST_CASE("title bar stylesheet parses", "[test_window_fit_and_title_bar][widget]") {
    capturedMessages.clear();
    const QtMessageHandler previous = qInstallMessageHandler(&captureMessages);
    auto host = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(host.get());
    auto* bar = new TitleBar(host.get());
    layout->addWidget(bar);
    host->show();
    QApplication::processEvents();
    qInstallMessageHandler(previous);
    host->close();

    for (const std::string& message : capturedMessages) {
        CHECK(message.find("Could not parse stylesheet") == std::string::npos);
    }
    CHECK_FALSE(bar->styleSheet().contains(QLatin1Char('@')));
}

TEST_CASE("the title bar names the product and the window buttons", "[widget]") {
    TitleBar bar;
    CHECK(bar.objectName() == QStringLiteral("customTitleBar"));
    CHECK(bar.titleLabel()->text() == QStringLiteral("OpenWhisper_CPP"));
    CHECK(bar.minimizeButton()->toolTip() == QStringLiteral("Minimize"));
    CHECK(bar.minimizeButton()->accessibleName() == QStringLiteral("Minimize window"));
    CHECK(bar.maximizeButton()->toolTip() == QStringLiteral("Maximize"));
    CHECK(bar.closeButton()->toolTip() == QStringLiteral("Close"));
    CHECK(bar.closeButton()->accessibleName() == QStringLiteral("Close window"));
    CHECK(bar.minimizeButton()->width() == TitleBar::kButtonWidth);
}

TEST_CASE("a notice sits just before the window buttons", "[widget]") {
    TitleBar bar;
    auto* notice = new QWidget;
    bar.addNotice(notice);
    auto* layout = static_cast<QHBoxLayout*>(bar.layout());
    CHECK(layout->indexOf(notice) + 1 == layout->indexOf(bar.minimizeButton()));
}

TEST_CASE("sync maximized follows the window without toggling it", "[widget]") {
    TitleBar bar;
    bar.syncMaximized(true);
    CHECK(bar.isMaximized());
    CHECK(bar.maximizeButton()->glyphName() == QStringLiteral("restore"));
    bar.syncMaximized(false);
    CHECK(bar.maximizeButton()->glyphName() == QStringLiteral("maximize"));
}

namespace {

void sendMouse(QWidget* target, QEvent::Type type, QPoint local, QPoint global,
               Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type, QPointF(local), QPointF(global), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

} // namespace

// CustomTitleBar's mouse handling (map row A01-43): a left-button drag on the bar moves the
// window by the drag, and a double-click toggles maximize.
TEST_CASE("dragging the title bar moves the window and a double-click maximizes it", "[widget]") {
    fvtest::AppTheme theme;
    Hosted hosted = hostedBar();
    QWidget* window = hosted.host.get();
    window->move(100, 120);
    QApplication::processEvents();
    const QPoint start = window->frameGeometry().topLeft();

    // On the title, clear of the menus, the window buttons and any resize edge.
    const QPoint local(hosted.bar->width() / 2, hosted.bar->height() / 2);
    const QPoint global = hosted.bar->mapToGlobal(local);
    sendMouse(hosted.bar, QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton);
    sendMouse(hosted.bar, QEvent::MouseMove, local + QPoint(40, 25), global + QPoint(40, 25),
              Qt::NoButton, Qt::LeftButton);
    sendMouse(hosted.bar, QEvent::MouseButtonRelease, local + QPoint(40, 25),
              global + QPoint(40, 25), Qt::LeftButton, Qt::NoButton);
    CHECK(window->frameGeometry().topLeft() == start + QPoint(40, 25));

    // A move without the button held does not drag.
    sendMouse(hosted.bar, QEvent::MouseMove, local, global, Qt::NoButton, Qt::NoButton);
    CHECK(window->frameGeometry().topLeft() == start + QPoint(40, 25));

    sendMouse(hosted.bar, QEvent::MouseButtonDblClick, local, global, Qt::LeftButton,
              Qt::LeftButton);
    CHECK(hosted.bar->isMaximized());
    CHECK(hosted.bar->maximizeButton()->glyphName() == QStringLiteral("restore"));
    sendMouse(hosted.bar, QEvent::MouseButtonDblClick, local, global, Qt::LeftButton,
              Qt::LeftButton);
    CHECK_FALSE(hosted.bar->isMaximized());
}
