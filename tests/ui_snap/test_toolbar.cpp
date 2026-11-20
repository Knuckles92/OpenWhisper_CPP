// The Snap toolbar's layout against OpenWhisper's _layout and _wrap (tests/golden/
// ui_qt.snap.toolbar.json), and its enable rules, tips and signals.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include <QFont>
#include <QFontMetrics>
#include <QWidget>

#include "base/json.h"
#include "golden/golden.h"
#include "ui_snap/annotations.h"
#include "ui_snap/toolbar.h"
#include "ui_theme/palette.h"

#include "overlay_test_support.h"

using namespace fv::ui_snap;
using namespace fvtest;
using fv::Json;

namespace {

// The cases' label font: Segoe UI at a pixel size.
QFontMetrics metrics(int pixelSize) {
    QFont font(QStringLiteral("Segoe UI"));
    font.setPixelSize(pixelSize);
    return QFontMetrics(font);
}

Json dump(const ToolbarLayout& layout) {
    Json buttons = Json::array();
    for (const ToolbarButton& button : layout.buttons) {
        buttons.push_back(
            Json::array({button.kind.toStdString(), button.value.toStdString(), button.rect.x(),
                         button.rect.y(), button.rect.width(), button.rect.height(),
                         button.tip.toStdString(), button.label.toStdString()}));
    }
    Json separators = Json::array();
    for (const QPoint& separator : layout.separators) {
        separators.push_back(Json::array({separator.x(), separator.y()}));
    }
    return Json{{"buttons", buttons}, {"separators", separators}};
}

MarkShortcuts shortcutsOf(const Json& values) {
    MarkShortcuts shortcuts;
    for (const auto& [key, value] : values.items()) {
        shortcuts.insert(QString::fromStdString(key),
                         QString::fromStdString(value.get<std::string>()));
    }
    return shortcuts;
}

} // namespace

TEST_CASE("_layout matches OpenWhisper", "[golden][snap_toolbar]") {
    fv::golden::for_each("ui_qt.snap.toolbar", "_layout", [](const fv::golden::Case& c) {
        const ToolbarLayout layout =
            toolbarLayout(c.arg<bool>(0), metrics(c.arg<int>(1)), shortcutsOf(c.arg<Json>(2)));
        CHECK(fv::golden::equal(c.result<Json>(), dump(layout)));
    });
}

TEST_CASE("_wrap matches OpenWhisper", "[golden][snap_toolbar]") {
    fv::golden::for_each("ui_qt.snap.toolbar", "_wrap", [](const fv::golden::Case& c) {
        const ToolbarLayout layout = toolbarLayout(c.arg<bool>(0), metrics(c.arg<int>(1)));
        CHECK(
            fv::golden::equal(c.result<Json>(), dump(wrapToolbar(layout.buttons, c.arg<int>(2)))));
    });
}

TEST_CASE("the toolbar is one row until it is fitted to a narrower width", "[snap_toolbar]") {
    SnapToolbar toolbar(nullptr, fv::ui::darkPalette());
    const std::vector<ToolbarButton> row = toolbar.buttons();
    CHECK(toolbar.objectName() == QStringLiteral("snapToolbar"));
    CHECK(toolbar.focusPolicy() == Qt::NoFocus);
    CHECK(toolbar.height() == kToolbarButton + 2 * kToolbarPad);
    CHECK(toolbar.width() == row.back().rect.right() + 1 + kToolbarPad);

    toolbar.fitToWidth(toolbar.width());
    CHECK(toolbar.buttons() == row);

    toolbar.fitToWidth(400);
    CHECK(toolbar.width() <= 400);
    CHECK(toolbar.height() > kToolbarButton + 2 * kToolbarPad);
    CHECK(toolbar.buttons() == wrapToolbar(row, 400).buttons);
}

TEST_CASE("controls enable by the tool, the history and the drop rule", "[snap_toolbar]") {
    SnapToolbar toolbar(nullptr, fv::ui::darkPalette());
    Spy<> undo(&toolbar, &SnapToolbar::undoClicked);
    Spy<> infill(&toolbar, &SnapToolbar::infillClicked);
    Spy<int> color(&toolbar, &SnapToolbar::colorChosen);
    const auto press = [&toolbar](const QString& kind, const QString& value = QString()) {
        click(&toolbar, center(toolbar.buttonRect(kind, value)));
    };

    press(QStringLiteral("undo"));
    press(QStringLiteral("infill")); // the arrow takes no infill
    CHECK(undo.size() == 0);
    CHECK(infill.size() == 0);

    toolbar.setState(SnapTool::Ellipse, 0, true, false);
    press(QStringLiteral("undo"));
    press(QStringLiteral("infill"));
    CHECK(undo.size() == 1);
    CHECK(infill.size() == 1);

    toolbar.setState(SnapTool::Redact, 0, true, false);
    press(QStringLiteral("color"), QStringLiteral("3"));
    CHECK(color.size() == 0);
    const ToolbarButton swatch = *std::find_if(
        toolbar.buttons().begin(), toolbar.buttons().end(),
        [](const ToolbarButton& button) { return button.kind == QStringLiteral("color"); });
    REQUIRE(swatch.kind == QStringLiteral("color"));
    CHECK(toolbar.tipFor(swatch) == QStringLiteral("Redactions always use an opaque cover."));

    toolbar.setState(SnapTool::Box, 0, true, false);
    press(QStringLiteral("color"), QStringLiteral("3"));
    REQUIRE(color.size() == 1);
    CHECK(std::get<0>(color.calls[0]) == 3);
}

TEST_CASE("a press released over another control activates nothing", "[snap_toolbar]") {
    SnapToolbar toolbar(nullptr, fv::ui::darkPalette());
    Spy<> kept(&toolbar, &SnapToolbar::keepClicked);
    Spy<> dropped(&toolbar, &SnapToolbar::dropClicked);
    const QPointF keep = center(toolbar.buttonRect(QStringLiteral("keep")));
    const QPointF drop = center(toolbar.buttonRect(QStringLiteral("drop")));

    mouse(&toolbar, QEvent::MouseButtonPress, keep.x(), keep.y());
    mouse(&toolbar, QEvent::MouseButtonRelease, drop.x(), drop.y(), Qt::NoButton);
    CHECK(kept.size() == 0);
    CHECK(dropped.size() == 0);

    // A double-click is two clicks.
    click(&toolbar, keep);
    click(&toolbar, keep, QEvent::MouseButtonDblClick);
    CHECK(kept.size() == 2);
}

TEST_CASE("the live toolbar's drop explains itself when there is nothing to put down",
          "[snap_toolbar]") {
    SnapToolbar toolbar(nullptr, fv::ui::darkPalette(), SnapToolbar::Variant::Live, {},
                        {{QStringLiteral("keep_marks"), QStringLiteral("Ctrl+K")}});
    Spy<> dropped(&toolbar, &SnapToolbar::dropClicked);
    Spy<> keepMarks(&toolbar, &SnapToolbar::keepMarksClicked);
    Spy<> region(&toolbar, &SnapToolbar::regionClicked);
    Spy<> clear(&toolbar, &SnapToolbar::clearClicked);
    CHECK(toolbar.testAttribute(Qt::WA_TranslucentBackground));
    CHECK(toolbar.buttonRect(QStringLiteral("undo")).isNull());
    CHECK(toolbar.buttonRect(QStringLiteral("keep")).isNull());

    toolbar.setDropEnabled(false);
    click(&toolbar, center(toolbar.buttonRect(QStringLiteral("drop"))));
    CHECK(dropped.size() == 0);
    const ToolbarButton& drop = toolbar.buttons().back();
    REQUIRE(drop.kind == QStringLiteral("drop"));
    CHECK(toolbar.tipFor(drop) ==
          QStringLiteral("No tool is selected, so there is nothing to put down. Cancel or Done "
                         "on the walkthrough column ends the walkthrough."));
    toolbar.setDropEnabled(true);
    CHECK(toolbar.tipFor(drop) ==
          QStringLiteral("Put the tool down and return to the desktop (Esc)"));
    click(&toolbar, center(toolbar.buttonRect(QStringLiteral("drop"))));
    click(&toolbar, center(toolbar.buttonRect(QStringLiteral("keep_marks"))));
    click(&toolbar, center(toolbar.buttonRect(QStringLiteral("region"))));
    click(&toolbar, center(toolbar.buttonRect(QStringLiteral("clear"))));
    CHECK(dropped.size() == 1);
    CHECK(keepMarks.size() == 1);
    CHECK(region.size() == 1);
    CHECK(clear.size() == 1);

    bool named = false;
    for (const ToolbarButton& button : toolbar.buttons()) {
        if (button.kind == QStringLiteral("keep_marks")) {
            named = button.tip == QStringLiteral("Keep marks (Ctrl+K)");
        }
    }
    CHECK(named);
}

TEST_CASE("the overlay toolbar has no delay slot", "[snap_toolbar]") {
    SnapToolbar toolbar(nullptr, fv::ui::darkPalette());
    auto* chip = new QWidget;
    toolbar.setCaptureDelay(chip);
    CHECK(toolbar.captureDelay() == nullptr);
    delete chip;
}
