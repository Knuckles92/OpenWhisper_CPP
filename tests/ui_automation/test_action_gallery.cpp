// PresetGallery (ui_qt/widgets/action_gallery.py): the starting points offered, in order, picked
// by a click or a key, two to a row from 600 px at the font scale.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QApplication>
#include <QGridLayout>
#include <QKeyEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>

#include "automation/action_model.h"
#include "automation_support.h"
#include "ui_automation/action_gallery.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::PresetGallery;
namespace automation = fv::automation;

std::vector<std::string> ids(const std::vector<automation::Preset>& presets) {
    std::vector<std::string> out;
    for (const automation::Preset& preset : presets) {
        out.push_back(preset.id);
    }
    return out;
}

} // namespace

TEST_CASE("the gallery offers every preset but the agent hotkey, with your own link", "[unit]") {
    const std::vector<automation::Preset> presets = fv::action_gallery::galleryPresets();
    CHECK(ids(presets) == std::vector<std::string>{"claude-chat", "chatgpt-app", "chatgpt-web",
                                                   "own-link", "quick-answer",
                                                   "claude-code-session", "webhook", "program",
                                                   "copy", "scripts"});
    const automation::Preset& own = fv::action_gallery::ownLink();
    CHECK(own.name == "Your own link");
    CHECK(own.kind == "open_link");
    CHECK(own.params == fv::Json{{"template", ""}});
    CHECK(own.description == "Open any web address or app link with your words in it.");
    CHECK(own.icon == "world-blue.svg");
    CHECK(own.note.empty());
}

TEST_CASE("a preset tile says what it starts and carries its caveat", "[widget]") {
    PresetGallery gallery;
    CHECK(gallery.tiles().size() == 10);
    fv::PresetTile* web = gallery.tile(QStringLiteral("chatgpt-web"));
    REQUIRE(web != nullptr);
    CHECK(web->property(fv::ui::prop::tileId) == QStringLiteral("actionPreset"));
    CHECK(web->property(fv::ui::prop::kind) == QStringLiteral("toggle"));
    CHECK(web->focusPolicy() == Qt::StrongFocus);
    CHECK(web->accessibleName() == QStringLiteral("ChatGPT on the web"));
    auto* note = web->body()->findChild<fv::WrappedLabel*>(QStringLiteral("actionHint"));
    REQUIRE(note != nullptr);
    CHECK(note->text() ==
          QStringLiteral("ChatGPT doesn't document this link, so it may stop working."));
    CHECK(note->property(fv::ui::prop::tone) == QStringLiteral("warning"));
    CHECK(gallery.tile(QStringLiteral("webhook"))->body()->isHidden());
    CHECK(gallery.tile(QStringLiteral("agent")) == nullptr);
    CHECK(gallery.property("settingsSearch").toBool() == false);
    CHECK(gallery.property("settingsSearch").isValid());
}

TEST_CASE("a click, Return, Enter or Space picks a starting point", "[widget]") {
    PresetGallery gallery;
    gallery.resize(680, 900);
    gallery.show();
    QApplication::processEvents();
    std::vector<std::string> picked;
    QObject::connect(&gallery, &PresetGallery::picked,
                     [&](const automation::Preset& preset) { picked.push_back(preset.id); });
    QTest::mouseClick(gallery.tile(QStringLiteral("webhook")), Qt::LeftButton);
    QTest::mouseClick(gallery.tile(QStringLiteral("webhook")), Qt::RightButton);
    QTest::keyClick(gallery.tile(QStringLiteral("program")), Qt::Key_Return);
    QTest::keyClick(gallery.tile(QStringLiteral("copy")), Qt::Key_Enter);
    QTest::keyClick(gallery.tile(QStringLiteral("scripts")), Qt::Key_Space);
    QTest::keyClick(gallery.tile(QStringLiteral("scripts")), Qt::Key_A);
    CHECK(picked == std::vector<std::string>{"webhook", "program", "copy", "scripts"});

    QSignalSpy cancelled(&gallery, &PresetGallery::cancelled);
    gallery.cancelLink()->click();
    CHECK(cancelled.count() == 1);
    CHECK(gallery.cancelLink()->toolTip() == QStringLiteral("Close without adding an action"));
}

TEST_CASE("starting points sit two to a row from 600 px at the font scale", "[widget]") {
    fvtest::AppTheme theme;
    PresetGallery gallery;
    gallery.resize(599, 1600);
    gallery.show();
    QApplication::processEvents();
    CHECK(gallery.columns() == 1);
    gallery.resize(600, 1600);
    QApplication::processEvents();
    CHECK(gallery.columns() == 2);
    int row = 0;
    int column = 0;
    int rowSpan = 0;
    int columnSpan = 0;
    gallery.grid()->getItemPosition(gallery.grid()->indexOf(gallery.tiles()[3]), &row, &column,
                                    &rowSpan, &columnSpan);
    CHECK(row == 1);
    CHECK(column == 1);

    // 600 x 1.3 = 780; a font change arranges again on the next pass.
    theme.setScale(130);
    QApplication::processEvents();
    gallery.resize(779, 1600);
    QApplication::processEvents();
    CHECK(gallery.columns() == 1);
    gallery.resize(780, 1600);
    QApplication::processEvents();
    CHECK(gallery.columns() == 2);
}
