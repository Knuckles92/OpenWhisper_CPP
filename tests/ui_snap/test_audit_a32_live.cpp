// Checklist line WT-43 of docs/port/map/a32-user-facing-features.md: when F8 or F9 belongs to
// another shortcut, the live toolbar's Keep marks and Clear marks name the key that still works
// while drawing. The tips are also in the _layout golden, which fails off Windows on label widths,
// so they are pinned here on their own.
#include <catch2/catch_test_macros.hpp>

#include <QString>

#include "ui_snap/toolbar.h"
#include "ui_theme/palette.h"

using namespace fv::ui_snap;

namespace {

QString tipOf(const SnapToolbar& toolbar, const QString& kind) {
    for (const ToolbarButton& button : toolbar.buttons()) {
        if (button.kind == kind) {
            return button.tip;
        }
    }
    return {};
}

} // namespace

TEST_CASE("without its function key a mark button names the key that works while drawing",
          "[snap_toolbar]") {
    SnapToolbar taken(nullptr, fv::ui::darkPalette(), SnapToolbar::Variant::Live, {}, {});
    CHECK(tipOf(taken, QStringLiteral("keep_marks")) ==
          QStringLiteral("Keep marks (K while drawing)"));
    CHECK(tipOf(taken, QStringLiteral("clear")) == QStringLiteral("Clear marks (C while drawing)"));

    SnapToolbar free(nullptr, fv::ui::darkPalette(), SnapToolbar::Variant::Live, {},
                     {{QStringLiteral("keep_marks"), QStringLiteral("F8")},
                      {QStringLiteral("clear"), QStringLiteral("F9")}});
    CHECK(tipOf(free, QStringLiteral("keep_marks")) == QStringLiteral("Keep marks (F8)"));
    CHECK(tipOf(free, QStringLiteral("clear")) == QStringLiteral("Clear marks (F9)"));
}
