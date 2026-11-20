// The column's Snap menu (ui_qt/widgets/walkthrough_snap_menu.py), from
// tests/test_walkthrough_panel.py's "the Snap menu" tests.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

#include <QGuiApplication>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSignalSpy>
#include <QString>

#include "panel_support.h"
#include "settings/enums.h"
#include "ui_walkthrough/walkthrough_snap_menu.h"

namespace fvtest {
namespace {

using fv::SnapMenu;
using fv::WalkthroughPanel;

// Poses as the Qt platform name for one test, as OpenWhisper's monkeypatching of
// pointed.supported does.
class PlatformNameScope {
public:
    explicit PlatformNameScope(const QString& name)
        : previous_(fv::ui_snap::setQtPlatformName(name)) {}
    ~PlatformNameScope() { fv::ui_snap::setQtPlatformName(previous_); }
    PlatformNameScope(const PlatformNameScope&) = delete;
    PlatformNameScope& operator=(const PlatformNameScope&) = delete;

private:
    std::optional<QString> previous_;
};

} // namespace

// from tests/test_walkthrough_panel.py::test_the_chevron_opens_the_menu_beside_the_column
TEST_CASE_METHOD(PanelFixture, "the chevron opens the menu beside the column",
                 "[test_walkthrough_panel][widget]") {
    QSignalSpy chosen(&panel, &WalkthroughPanel::snapModeChosen);
    panel.begin();
    panel.setSnapMode(QStringLiteral("screen"), QStringLiteral("Ctrl+Alt+S"));
    panel.grab();

    click(panel, footerButton("snap-menu"));
    SnapMenu* menu = panel.menu();
    REQUIRE(menu != nullptr);
    CHECK(menu->isVisible());
    CHECK(menu->current() == QStringLiteral("screen"));
    CHECK(menu->shortcut() == QStringLiteral("Ctrl+Alt+S"));
    // Beside the column, not over it.
    CHECK(menu->geometry().right() < panel.mapToGlobal(panel.card().topLeft().toPoint()).x() + 20);
    menu->grab();

    const auto index = menu->modes().indexOf(QStringLiteral("delayed"));
    REQUIRE(index >= 0);
    click(*menu, menu->item(static_cast<int>(index)).center());
    REQUIRE(chosen.size() == 1);
    CHECK(chosen.at(0).at(0).toString() == QStringLiteral("delayed"));
}

// from tests/test_walkthrough_panel.py::test_the_menu_offers_a_window_only_where_one_can_be_found
TEST_CASE_METHOD(PanelFixture, "the menu offers a window only where one can be found",
                 "[test_walkthrough_panel][widget]") {
    {
        PlatformNameScope offscreen(QStringLiteral("offscreen"));
        CHECK_FALSE(panel.snapModes().contains(QStringLiteral("window")));
    }
    PlatformNameScope windows(QStringLiteral("windows"));
    CHECK(panel.snapModes() == QStringList{QStringLiteral("region"), QStringLiteral("window"),
                                           QStringLiteral("screen"), QStringLiteral("delayed")});
}

// from tests/test_walkthrough_panel.py::test_the_countdown_shows_on_snap
TEST_CASE_METHOD(PanelFixture, "the countdown shows on snap", "[test_walkthrough_panel][widget]") {
    panel.begin();
    panel.setCountdown(2);
    panel.grab();
    CHECK(panel.countdown() == 2);
    panel.finish();
    CHECK(panel.countdown() == 0);
}

// from tests/test_walkthrough_panel.py::test_the_menu_never_takes_the_focus
TEST_CASE_METHOD(PanelFixture, "the menu never takes the focus",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    panel.grab();
    click(panel, footerButton("snap-menu"));
    REQUIRE(panel.menu() != nullptr);
    const Qt::WindowFlags flags = panel.menu()->windowFlags();

    CHECK(flags.testFlag(Qt::WindowDoesNotAcceptFocus));
    // A popup takes the focus on Windows, whatever it is told.
    CHECK(panel.menu()->windowType() == Qt::Tool);
    CHECK(panel.menu()->testAttribute(Qt::WA_ShowWithoutActivating));
}

// from tests/test_walkthrough_panel.py::
//     test_the_chevron_again_or_another_click_in_the_column_closes_the_menu
TEST_CASE_METHOD(PanelFixture, "the chevron again or another click in the column closes the menu",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    panel.grab();
    click(panel, footerButton("snap-menu"));
    click(panel, footerButton("snap-menu"));
    pumpEvents();
    CHECK(panel.menu() == nullptr);

    click(panel, footerButton("snap-menu"));
    REQUIRE(panel.menu() != nullptr);
    click(panel, QPointF(panel.inner().left() + 10, panel.inner().top() + 10));
    pumpEvents();
    CHECK(panel.menu() == nullptr);
}

// from tests/test_walkthrough_panel.py::test_the_menu_closes_once_the_pointer_has_wandered_off
TEST_CASE_METHOD(PanelFixture, "the menu closes once the pointer has wandered off",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    panel.grab();
    click(panel, footerButton("snap-menu"));
    SnapMenu* menu = panel.menu();
    REQUIRE(menu != nullptr);
    menu->setNearRect(QRect());
    menu->setPointerSource(
        [menu] { return menu->geometry().topLeft() - QRect(0, 0, 50, 50).bottomRight(); });
    const auto half = std::chrono::milliseconds(300);

    menu->checkPointer();
    clock.advance(half);
    menu->checkPointer();
    CHECK(menu->isVisible());
    clock.advance(2 * half);
    menu->checkPointer();
    pumpEvents();
    CHECK(panel.menu() == nullptr);
}

TEST_CASE_METHOD(PanelFixture, "the pointer on the menu or the column keeps it open",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    panel.grab();
    click(panel, footerButton("snap-menu"));
    SnapMenu* menu = panel.menu();
    REQUIRE(menu != nullptr);
    // Opened from the column, the column is where the pointer may wander.
    CHECK(menu->nearRect() == panel.frameGeometry());
    QPoint pointer = panel.frameGeometry().center();
    menu->setPointerSource([&pointer] { return pointer; });
    for (int i = 0; i < 20; ++i) {
        menu->checkPointer();
        clock.advance(std::chrono::milliseconds(100));
    }
    CHECK(menu->isVisible());
    pointer = menu->geometry().center();
    for (int i = 0; i < 20; ++i) {
        menu->checkPointer();
        clock.advance(std::chrono::milliseconds(100));
    }
    CHECK(menu->isVisible());
    // Away, then back before 0.6 s: the time away starts again.
    pointer = QPoint(-5000, -5000);
    menu->checkPointer();
    clock.advance(std::chrono::milliseconds(500));
    pointer = menu->geometry().center();
    menu->checkPointer();
    pointer = QPoint(-5000, -5000);
    menu->checkPointer();
    clock.advance(std::chrono::milliseconds(500));
    menu->checkPointer();
    CHECK(menu->isVisible());
}

TEST_CASE("the menu opens left of the column, else right of it, and never above the screen",
          "[walkthrough_panel][widget]") {
    FakeDesktopScope desktop;
    fv::FakeClock clock;
    QScreen* screen = QGuiApplication::primaryScreen();
    REQUIRE(screen != nullptr);
    const QRect area = screen->availableGeometry();
    const QStringList modes{QStringLiteral("region"), QStringLiteral("screen")};

    // Room on the left: 10 px from the card, its foot level with bottom.
    auto* left = new SnapMenu(clock, modes, QStringLiteral("region"));
    const QRect card(area.right() - 300, area.top() + 100, 296, 400);
    left->openBeside(card, area.top() + 450);
    CHECK(left->x() == card.left() - SnapMenu::kGap - SnapMenu::kWidth - SnapMenu::kShadow);
    CHECK(left->y() == area.top() + 450 - left->height() + SnapMenu::kShadow);
    // With no near rectangle, the card is where the pointer may wander.
    CHECK(left->nearRect() == card);
    left->close();

    // At the screen's left edge it opens right of the card, held below the screen's top.
    auto* right = new SnapMenu(clock, modes, QStringLiteral("region"));
    const QRect edge(area.left() + 4, area.top() + 10, 296, 400);
    right->openBeside(edge, area.top() + 20, QRect(0, 0, 10, 10));
    CHECK(right->x() == edge.right() + SnapMenu::kGap - SnapMenu::kShadow);
    CHECK(right->y() == area.top() - SnapMenu::kShadow);
    CHECK(right->nearRect() == QRect(0, 0, 10, 10));
    right->close();
    pumpEvents();
}

TEST_CASE("the menu lists each kind by its label, the current one ticked",
          "[walkthrough_panel][widget]") {
    fv::FakeClock clock;
    const QStringList modes{QStringLiteral("region"), QStringLiteral("screen"),
                            QStringLiteral("delayed")};
    SnapMenu menu(clock, modes, QStringLiteral("region"), QStringLiteral("Ctrl+Shift+S"));
    CHECK(menu.size() == QSize(250 + 24, 5 + 26 + 36 * 3 + 5 + 24));
    CHECK(fv::snapModeLabel(QStringLiteral("region")) == QStringLiteral("Region"));
    CHECK(fv::snapModeLabel(QStringLiteral("window")) ==
          QStringLiteral("Window under the pointer"));
    CHECK(fv::snapModeLabel(QStringLiteral("screen")) == QStringLiteral("Whole screen"));
    CHECK(fv::snapModeLabel(QStringLiteral("delayed")) == QStringLiteral("In 3 seconds"));
    CHECK(menu.item(1) == QRectF(12 + 5, 12 + 5 + 26 + 36, 250 - 10, 36));
    CHECK(menu.testAttribute(Qt::WA_DeleteOnClose));
    CHECK(menu.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
    menu.setAttribute(Qt::WA_DeleteOnClose, false);
}

} // namespace fvtest
