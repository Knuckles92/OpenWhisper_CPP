// The shell's chrome against the references OpenWhisper rendered for it
// (tools/parity/builders/a01.py, tests/reference/<widget>/<theme>-windows11-dpr1-s<scale>):
// the widget tree first, then the pixels, with the application styled as OpenWhisper styles it.
// The builders put FioriVoce's name where OpenWhisper paints its own.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>

#include <QApplication>
#include <QEvent>
#include <QMenuBar>
#include <QRect>
#include <QStyle>
#include <QToolButton>

#include "screenshot/screenshot.h"
#include "shell_fixture.h"
#include "ui_shell/footer_bar.h"
#include "ui_shell/loading_screen.h"
#include "ui_shell/main_window.h"
#include "ui_shell/title_bar.h"

namespace {

void check(const std::string& widget, const std::string& theme, int scale,
           const std::function<std::unique_ptr<QWidget>()>& build,
           std::vector<std::string> ignore = {}) {
    fvtest::AppTheme appTheme(QString::fromStdString(theme), scale);
    std::unique_ptr<QWidget> built = build();
    fv::screenshot::Options options;
    options.ignore_keys = std::move(ignore);
    fv::screenshot::expect_matches(*built, widget,
                                   theme + "-windows11-dpr1-s" + std::to_string(scale), options);
}

// CustomTitleBar standalone, not maximized, no notice, with the three menus. The class is
// TitleBar here (architecture 2.3); its Qt class, QFrame, is compared.
std::unique_ptr<QWidget> titleBar() {
    auto bar = std::make_unique<fv::TitleBar>();
    for (const char* name : {"File", "View", "Help"}) {
        bar->menuBar()->addMenu(QString::fromLatin1(name));
    }
    // As the builder does: polish the menu bar, then let the bar fit its height to it (the
    // LayoutRequest handler is the builder's _sync_height call).
    bar->menuBar()->ensurePolished();
    QEvent layoutRequest(QEvent::LayoutRequest);
    QApplication::sendEvent(bar.get(), &layoutRequest);

    // The menu bar sizes its extension button by the style's PM_ToolBarExtensionExtent, which
    // windows11 gives as 32 in Qt 6.11 (the kit that rendered the references) and as 12 in Qt
    // 6.10. The button stays hidden, since the three menus fit, so the kits differ only in the
    // geometry the tree records for it: on a kit with the narrower extent it is laid out as
    // Qt 6.11 lays it out, against the same right edge.
    constexpr int kQt611Extent = 32;
    QMenuBar* menuBar = bar->menuBar();
    auto* extension = menuBar->findChild<QToolButton*>(QStringLiteral("qt_menubar_ext_button"));
    if (extension != nullptr && menuBar->style()->pixelMetric(QStyle::PM_ToolBarExtensionExtent,
                                                              nullptr, menuBar) != kQt611Extent) {
        const QRect laid = extension->geometry();
        extension->setGeometry(laid.right() + 1 - kQt611Extent, laid.y(), kQt611Extent,
                               laid.height());
    }
    return bar;
}

// The main window's footer with a tray available, grabbed on its own.
std::unique_ptr<QWidget> footerBar() {
    fvtest::ShellFixture shell;
    auto window = shell.window();
    auto footer = std::unique_ptr<QWidget>(window->footer());
    footer->setParent(nullptr);
    return footer;
}

// The classic splash at a pinned phase (pulse 0.5) with the first startup step's lines.
std::unique_ptr<QWidget> loadingScreen() {
    auto screen = std::make_unique<fv::LoadingScreen>();
    screen->pinGlowPhase(0.0);
    screen->updateStatus(QStringLiteral("Initializing components..."));
    screen->updateProgress(QStringLiteral("Preparing startup..."));
    return screen;
}

} // namespace

TEST_CASE("reference screenshot title_bar dark", "[screenshot]") {
    check("title_bar", "dark", 100, titleBar, {"class"});
}

TEST_CASE("reference screenshot title_bar light", "[screenshot]") {
    check("title_bar", "light", 100, titleBar, {"class"});
}

TEST_CASE("reference screenshot title_bar dark 130", "[screenshot]") {
    check("title_bar", "dark", 130, titleBar, {"class"});
}

TEST_CASE("reference screenshot footer_bar dark", "[screenshot]") {
    check("footer_bar", "dark", 100, footerBar);
}

TEST_CASE("reference screenshot footer_bar light", "[screenshot]") {
    check("footer_bar", "light", 100, footerBar);
}

TEST_CASE("reference screenshot footer_bar dark 130", "[screenshot]") {
    check("footer_bar", "dark", 130, footerBar);
}

TEST_CASE("reference screenshot loading_screen dark", "[screenshot]") {
    check("loading_screen", "dark", 100, loadingScreen);
}

TEST_CASE("reference screenshot loading_screen light", "[screenshot]") {
    check("loading_screen", "light", 100, loadingScreen);
}