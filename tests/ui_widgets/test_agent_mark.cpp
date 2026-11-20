// AgentMark and the agent painting helpers (ui_qt/widgets/agent_picker.py:253-378): the eases,
// colour mixing, the mark's size and state, and its pixels against the references OpenWhisper
// rendered (tools/parity/builders/a21.py: Claude Code's mark in the dark theme, Codex's in the
// light one, both at warmth 1.0).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFontMetricsF>

#include "app_theme.h"
#include "screenshot/screenshot.h"
#include "ui_widgets/agent_mark.h"

namespace paint = fv::agent_paint;
using Catch::Approx;

TEST_CASE("the agent eases run from 0 to 1 and clamp outside", "[unit]") {
    CHECK(paint::clamp(-0.5) == 0.0);
    CHECK(paint::clamp(0.25) == 0.25);
    CHECK(paint::clamp(3.0) == 1.0);
    CHECK(paint::easeOut(0.0) == 0.0);
    CHECK(paint::easeOut(1.0) == 1.0);
    CHECK(paint::easeOut(0.5) == Approx(0.875));
    CHECK(paint::easeOut(-1.0) == 0.0);
    CHECK(paint::easeOut(2.0) == 1.0);
    CHECK(paint::easeInOut(0.0) == 0.0);
    CHECK(paint::easeInOut(0.25) == Approx(0.0625));
    CHECK(paint::easeInOut(0.5) == Approx(0.5));
    CHECK(paint::easeInOut(0.75) == Approx(0.9375));
    CHECK(paint::easeInOut(1.0) == 1.0);
}

TEST_CASE("agent colours mix channel by channel and take an alpha", "[unit]") {
    const QColor black(0, 0, 0, 0);
    const QColor white(255, 255, 255, 255);
    // 127.5 rounds half to even.
    CHECK(paint::mix(black, white, 0.5) == QColor(128, 128, 128, 128));
    CHECK(paint::mix(QColor(1, 3, 5, 7), QColor(2, 4, 6, 8), 0.5) == QColor(2, 4, 6, 8));
    CHECK(paint::mix(black, white, 2.0) == white);
    CHECK(paint::mix(black, white, -1.0) == black);
    const QColor accent(QStringLiteral("#D97757"));
    CHECK(paint::alpha(accent, 0.4).alpha() == 102);
    CHECK(paint::alpha(accent, 1.5).alpha() == 255);
    CHECK(paint::alpha(accent, 0.4).red() == accent.red());
}

TEST_CASE("the ring colour follows the theme", "[widget]") {
    const QColor accent(QStringLiteral("#10A37F"));
    {
        fvtest::AppTheme theme(QStringLiteral("dark"));
        CHECK(paint::dark());
        CHECK(paint::ringColor(accent) == accent.lighter(118));
    }
    {
        fvtest::AppTheme theme(QStringLiteral("light"));
        CHECK_FALSE(paint::dark());
        CHECK(paint::ringColor(accent) == accent.darker(112));
    }
}

TEST_CASE("an agent mark is twice the font's height and at least 24 px", "[widget]") {
    fv::AgentMark mark(QStringLiteral("CC"), QColor(QStringLiteral("#D97757")));
    CHECK(mark.objectName() == QStringLiteral("agentMark"));
    CHECK(mark.testAttribute(Qt::WA_TransparentForMouseEvents));
    const double height = QFontMetricsF(mark.font()).height();
    CHECK(mark.side() == std::max(24, static_cast<int>(std::lround(height * 2.0))));
    CHECK(mark.sizeHint() == QSize(mark.side(), mark.side()));
    CHECK(mark.minimumSizeHint() == mark.sizeHint());
    QFont tiny = mark.font();
    tiny.setPixelSize(6);
    mark.setFont(tiny);
    CHECK(mark.side() == 24);
}

TEST_CASE("an agent mark keeps its state for callers", "[widget]") {
    fv::AgentMark mark(QStringLiteral("CC"), QColor(QStringLiteral("#D97757")));
    CHECK(mark.warmth() == 0.0);
    CHECK(mark.missing() == 0.0);
    mark.setWarmth(0.4);
    CHECK(mark.warmth() == 0.4);
    mark.setWarmth(7.0);
    CHECK(mark.warmth() == 1.0);
    mark.setMissing(-2.0);
    CHECK(mark.missing() == 0.0);
    mark.setMissing(1.0);
    CHECK(mark.missing() == 1.0);
    mark.setMonogram(QStringLiteral("Cx"));
    CHECK(mark.monogram() == QStringLiteral("Cx"));
    CHECK(mark.accent() == QColor(QStringLiteral("#D97757")));
}

TEST_CASE("reference screenshot agent_mark/claude-dark", "[screenshot]") {
    fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::AgentMark mark(QStringLiteral("CC"), QColor(QStringLiteral("#D97757")));
    mark.setWarmth(1.0);
    fv::screenshot::expect_matches(mark, "agent_mark/claude-dark", "dark-windows11-dpr1-s100");
}

TEST_CASE("reference screenshot agent_mark/codex-light", "[screenshot]") {
    fvtest::AppTheme theme(QStringLiteral("light"));
    fv::AgentMark mark(QStringLiteral("Cx"), QColor(QStringLiteral("#10A37F")));
    mark.setWarmth(1.0);
    fv::screenshot::expect_matches(mark, "agent_mark/codex-light", "light-windows11-dpr1-s100");
}
