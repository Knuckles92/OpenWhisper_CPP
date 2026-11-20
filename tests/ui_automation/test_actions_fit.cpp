// Settings > Automate > Actions fits a narrow window at every text size
// (tests/test_automation_ui_actions_fit.py). OpenWhisper's test runs the classic look at 720 px
// and the Omarchy look at 720 and 460 px, all at 130 %; the Omarchy look is Linux-only
// (other-platform), so here the classic window runs at its narrowest at 90, 100, 115 and 130 %
// text, each with the page in every state the Python test opens.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QAbstractButton>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPoint>

#include "actions_page_support.h"
#include "automation_support.h"
#include "ui_automation/action_list.h"
#include "ui_settings/settings_page.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;

namespace {

Json actionsList() {
    return Json::array({
        Json{{"id", "agent"},
             {"name", "Agent hotkey"},
             {"kind", "agent"},
             {"params",
              {{"agent", "claude_code"},
               {"folder", "C:\\code\\a-rather-long-project-folder-name"},
               {"include_selection", true}}},
             {"hotkey", "ctrl+alt+shift+space"},
             {"spoken_names", Json::array({"Claude Code"})}},
        Json{{"id", "a1"},
             {"name", "Ask Claude"},
             {"kind", "open_link"},
             {"params", {{"template", "claude://claude.ai/new?q={text}"}}},
             {"hotkey", "ctrl+alt+c"},
             {"spoken_names", Json::array({"Claude"})}},
        Json{{"id", "h1"},
             {"name", "Home Assistant"},
             {"kind", "webhook"},
             {"params",
              {{"url", "http://homeassistant.local:8123/api/webhook/voice-commands"},
               {"output", "notice"}}},
             {"spoken_names", Json::array({"House"})},
             {"description", "Lights, heating and the kettle"}},
        Json{{"id", "p1"},
             {"name", "Journal"},
             {"kind", "program"},
             {"params",
              {{"program", "C:\\Tools\\journal.exe"}, {"args", Json::array({"--append"})}}},
             {"enabled", false}},
    });
}

struct Rect {
    int left, top, right, bottom;
};

Rect rectIn(const QWidget* widget, const QWidget* ancestor) {
    const QPoint topLeft = widget->mapTo(ancestor, widget->rect().topLeft());
    const QPoint bottomRight = widget->mapTo(ancestor, widget->rect().bottomRight());
    return {topLeft.x(), topLeft.y(), bottomRight.x(), bottomRight.y()};
}

std::vector<QWidget*> controls(const QWidget* widget) {
    std::vector<QWidget*> found;
    for (QWidget* child : widget->findChildren<QWidget*>()) {
        if ((qobject_cast<QAbstractButton*>(child) != nullptr ||
             qobject_cast<QLineEdit*>(child) != nullptr ||
             qobject_cast<QComboBox*>(child) != nullptr) &&
            child->isVisible()) {
            found.push_back(child);
        }
    }
    return found;
}

std::string describe(const QWidget* control) {
    if (const auto* button = qobject_cast<const QAbstractButton*>(control);
        button != nullptr && !control->objectName().isEmpty()) {
        return control->objectName().toStdString();
    }
    if (const auto* button = qobject_cast<const QAbstractButton*>(control)) {
        return button->text().toStdString();
    }
    return control->accessibleName().toStdString();
}

void open(fvtest::ActionsRig& rig, const std::string& state) {
    fv::ActionsPanel& panel = rig.panel();
    if (state == "new-folder") {
        fv::AgentFields& fields = *rig.card().fields();
        fields.chooser()->choice(QStringLiteral("codex"))->click();
        fields.newFolderRow()->switchButton()->click();
    } else if (state == "gallery") {
        panel.addAction();
    } else if (state != "list") {
        panel.addAction();
        fv::PresetTile* tile = panel.gallery()->tile(QString::fromStdString(state));
        REQUIRE(tile != nullptr);
        panel.startFromPreset(tile->preset());
    }
}

void checkFits(int scale, const std::string& state) {
    fvtest::AppTheme theme(QStringLiteral("dark"), scale);
    fvtest::ActionsRig rig(Json{{"actions", actionsList()}});
    fv::SettingsDialog& dialog = rig.dialog();
    open(rig, state);
    dialog.fitToScreen();
    // Clamped to the window's minimum width, as OpenWhisper's classic window is.
    dialog.resize(720, 600);
    fvtest::pump(12);
    QWidget* page = dialog.page(fvtest::dest::actions);
    REQUIRE(page != nullptr);
    CHECK(rig.card().isVisible());
    std::vector<fv::TileBase*> tiles;
    for (fv::TileBase* tile : page->findChildren<fv::TileBase*>()) {
        if (tile->isVisible()) {
            tiles.push_back(tile);
        }
    }
    CHECK_FALSE(tiles.empty());
    for (const fv::TileBase* tile : tiles) {
        for (const QWidget* control : controls(tile)) {
            const Rect rect = rectIn(control, tile);
            INFO(tile->titleLabel()->text().toStdString() << ": " << describe(control));
            CHECK(rect.left >= 0);
            CHECK(rect.right < tile->width());
        }
    }
    for (const QWidget* control : controls(page)) {
        INFO(describe(control));
        CHECK(rectIn(control, page).right < page->width());
    }
    // Nothing is squeezed below the height its text needs: Settings pages lay out at their
    // minimum height.
    CHECK(page->height() >= page->minimumSizeHint().height());
    for (const QLabel* label : page->findChildren<QLabel*>()) {
        if (label->isVisible() && label->wordWrap()) {
            INFO(label->text().toStdString());
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
    }
    for (const QWidget* control : controls(page)) {
        if (const auto* button = qobject_cast<const QAbstractButton*>(control);
            button != nullptr && !button->text().isEmpty()) {
            INFO(button->text().toStdString());
            CHECK(button->height() >= button->minimumSizeHint().height());
        }
    }
}

} // namespace

// from
// tests/test_automation_ui_actions_fit.py::test_the_actions_page_fits_narrow_windows_and_large_fonts
TEST_CASE("test_the_actions_page_fits_narrow_windows_and_large_fonts",
          "[test_automation_ui_actions_fit][widget]") {
    for (const int scale : {90, 100, 115, 130}) {
        for (const char* state :
             {"list", "new-folder", "gallery", "quick-answer", "webhook", "program"}) {
            INFO("scale " << scale << ", " << state);
            checkFits(scale, state);
        }
    }
}
