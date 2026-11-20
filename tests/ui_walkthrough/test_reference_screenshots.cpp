// The live column and its Snap menu against OpenWhisper's pixels (tools/parity/builders/a23.py,
// tests/reference/walkthrough_panel/<state>/ and tests/reference/walkthrough_snap_menu/). The
// script is the builder's, on a FakeClock in place of the pinned time.monotonic() and recording
// clock.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <QApplication>
#include <QImage>
#include <QString>

#include "base/testing/fake_clock.h"
#include "panel_support.h"
#include "screenshot/screenshot.h"
#include "testkit/testkit.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_walkthrough/walkthrough_panel.h"
#include "ui_walkthrough/walkthrough_snap_menu.h"

namespace fvtest {
namespace {

using fv::WalkthroughPanel;
using fv::ui_walkthrough::kTickS;

// The builder's SIZE and LEVELS.
constexpr int kWidth = 324;
constexpr int kHeight = 848;
constexpr std::array<double, 12> kLevels{0.05, 0.02, 0.08, 0.03,  0.1,  0.012,
                                         0.06, 0.04, 0.2,  0.025, 0.07, 0.009};

// The application styled the way OpenWhisper's render_reference.py styles it.
class AppTheme {
public:
    explicit AppTheme(const QString& theme) : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(100, *qApp, manager_);
    }
    ~AppTheme() { qApp->setStyleSheet(QString()); }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

// The builder's _Script: one clock for time.monotonic() and the recording's elapsed time.
class Script {
public:
    Script() = default;
    ~Script() {
        panel.stopFrames();
        panel.hide();
    }
    Script(const Script&) = delete;
    Script& operator=(const Script&) = delete;

    void at(int ms) {
        clock.advance(std::chrono::milliseconds(ms - ms_));
        ms_ = ms;
    }

    void speak(int count) {
        const double now = std::chrono::duration<double>(clock.steady().time_since_epoch()).count();
        for (int i = 0; i < count; ++i) {
            panel.column().hear(kLevels[static_cast<std::size_t>(written_) % kLevels.size()]);
            panel.column().advance(kTickS, now);
            ++written_;
        }
    }

    void shot(int number, double keptAtS) {
        const auto path = fv::testkit::source_root() / "tests" / "fixtures" / "walkthrough" /
                          ("shot-" + std::to_string(number) + ".png");
        const QImage picture(QString::fromStdWString(path.wstring()));
        REQUIRE_FALSE(picture.isNull());
        at(static_cast<int>(keptAtS * 1000));
        panel.addShot(number, picture, keptAtS);
    }

    // The builder's _recorded: the walkthrough read at 1:15.
    void record(bool holdAtEnd = false) {
        at(0);
        panel.begin();
        panel.setFixedSize(kWidth, kHeight);
        for (int step = 1; step <= 4; ++step) {
            at(500 * step);
            panel.advanceFrame();
        }
        speak(24);
        shot(1, 9.0);
        speak(30);
        at(20'000);
        panel.hold(true);
        speak(12);
        at(26'000);
        panel.hold(false);
        speak(10);
        shot(2, 48.0);
        speak(20);
        if (holdAtEnd) {
            at(70'000);
            panel.hold(true);
            speak(8);
        }
        at(75'000);
    }

    FakeDesktopScope desktop;
    fv::FakeClock clock;
    WalkthroughPanel panel{clock};

private:
    int ms_ = 0;
    int written_ = 0;
};

enum class Panel { Recording, Paused, Holding, Writing, Done, Hover };

void expectPanel(Panel state, std::string_view name, const char* theme) {
    AppTheme applied(QString::fromLatin1(theme));
    Script script;
    WalkthroughPanel& panel = script.panel;
    script.record(state == Panel::Holding);
    switch (state) {
    case Panel::Recording:
    case Panel::Holding:
        break;
    case Panel::Paused:
        panel.setPaused(true);
        break;
    case Panel::Writing:
        panel.finish();
        script.at(76'000);
        break;
    case Panel::Done:
        panel.finish();
        script.at(77'000);
        panel.complete(2, 75.0);
        script.at(78'000);
        break;
    case Panel::Hover: {
        panel.grab(); // draws the screenshots where the pointer finds them
        const std::optional<QRectF> first = panel.shotRect(1);
        REQUIRE(first);
        panel.setHover(first->center());
        break;
    }
    }
    fv::screenshot::expect_matches(panel, name, std::string(theme) + "-windows11-dpr1-s100");
}

void expectMenu(const char* theme) {
    AppTheme applied(QString::fromLatin1(theme));
    fv::FakeClock clock;
    fv::SnapMenu menu(clock,
                      {QStringLiteral("region"), QStringLiteral("window"), QStringLiteral("screen"),
                       QStringLiteral("delayed")},
                      QStringLiteral("region"), QStringLiteral("Ctrl+Alt+S"));
    fv::screenshot::expect_matches(menu, "walkthrough_snap_menu",
                                   std::string(theme) + "-windows11-dpr1-s100");
    menu.hide();
}

} // namespace

TEST_CASE("the recording column matches the reference, dark", "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Recording, "walkthrough_panel/recording", "dark");
}
TEST_CASE("the recording column matches the reference, light", "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Recording, "walkthrough_panel/recording", "light");
}
TEST_CASE("the paused column matches the reference, dark", "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Paused, "walkthrough_panel/paused", "dark");
}
TEST_CASE("the paused column matches the reference, light", "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Paused, "walkthrough_panel/paused", "light");
}
TEST_CASE("the column holding a note matches the reference, dark",
          "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Holding, "walkthrough_panel/holding", "dark");
}
TEST_CASE("the column holding a note matches the reference, light",
          "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Holding, "walkthrough_panel/holding", "light");
}
TEST_CASE("the column writing it up matches the reference, dark",
          "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Writing, "walkthrough_panel/writing", "dark");
}
TEST_CASE("the column writing it up matches the reference, light",
          "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Writing, "walkthrough_panel/writing", "light");
}
TEST_CASE("the finished column matches the reference, dark", "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Done, "walkthrough_panel/done", "dark");
}
TEST_CASE("the finished column matches the reference, light", "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Done, "walkthrough_panel/done", "light");
}
TEST_CASE("a hovered screenshot's tools match the reference, dark",
          "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Hover, "walkthrough_panel/hover", "dark");
}
TEST_CASE("a hovered screenshot's tools match the reference, light",
          "[screenshot][walkthrough_panel]") {
    expectPanel(Panel::Hover, "walkthrough_panel/hover", "light");
}
TEST_CASE("the snap menu matches the reference, dark", "[screenshot][walkthrough_snap_menu]") {
    expectMenu("dark");
}
TEST_CASE("the snap menu matches the reference, light", "[screenshot][walkthrough_snap_menu]") {
    expectMenu("light");
}

} // namespace fvtest
