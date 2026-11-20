// The small fields of ui_qt/widgets/action_fields.py: the helper constructors, Columns,
// StatusLine, PathField, ShortcutField, RouteShortcutField, SwitchRow and OutputCombo.
#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

#include <QApplication>
#include <QBoxLayout>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "automation_support.h"
#include "base/json.h"
#include "ui_automation/action_fields.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace {

using fv::Json;
namespace fields = fv::action_fields;

// Replaces a picker for one test case.
class ScopedChooser {
public:
    ScopedChooser(fields::PathChooser& slot, fields::PathChooser replacement)
        : slot_(slot), saved_(std::exchange(slot, std::move(replacement))) {}
    ~ScopedChooser() { slot_ = std::move(saved_); }
    ScopedChooser(const ScopedChooser&) = delete;
    ScopedChooser& operator=(const ScopedChooser&) = delete;

private:
    fields::PathChooser& slot_;
    fields::PathChooser saved_;
};

// The Settings window's standard-shortcut path, recorded.
class FakeShortcutHost final : public fv::StandardShortcutHost {
public:
    Json settings() override { return saved; }
    Json currentHotkeys() override { return current; }
    QString setStandardHotkey(const QString& action, const QString& hotkey) override {
        calls.emplace_back(action, hotkey);
        if (!refusal.isEmpty()) {
            return refusal;
        }
        current[fv::qt::toUtf8(action)] = fv::qt::toUtf8(hotkey);
        return {};
    }
    void setHotkeyCaptureSuspended(bool suspended) override { suspends.push_back(suspended); }

    Json saved = Json::object();
    Json current = Json::object();
    QString refusal;
    std::vector<std::pair<QString, QString>> calls;
    std::vector<bool> suspends;
};

} // namespace

TEST_CASE("the field helpers carry the theme's names", "[widget]") {
    auto* buddy = new QLineEdit;
    std::unique_ptr<QLabel> label(fields::fieldLabel(QStringLiteral("Model"), buddy));
    CHECK(label->objectName() == QStringLiteral("textModelFieldLabel"));
    CHECK(label->buddy() == buddy);
    delete buddy;
    std::unique_ptr<fv::WrappedLabel> hint(fields::hintLabel(QStringLiteral("x")));
    CHECK(hint->objectName() == QStringLiteral("actionHint"));
    CHECK(hint->textFormat() == Qt::PlainText);
    std::unique_ptr<QPushButton> link(
        fields::pageLink(QStringLiteral("Open"), QStringLiteral("t")));
    CHECK(link->objectName() == QStringLiteral("actionLink"));
    CHECK(link->isFlat());
    CHECK(link->toolTip() == QStringLiteral("t"));
    std::unique_ptr<QLineEdit> input(fields::inputField(new QLineEdit));
    CHECK(input->property(fv::ui::prop::tileId) == QStringLiteral("actionInput"));
    std::unique_ptr<QWidget> heading(fields::sectionHeading(QStringLiteral("Your words")));
    CHECK(heading->findChild<QFrame*>(QStringLiteral("actionDivider"))->height() == 1);
    CHECK(heading->findChild<QLabel*>(QStringLiteral("actionSectionLabel"))->text() ==
          QStringLiteral("Your words"));
    auto* a = new QLabel;
    auto* b = new QLabel;
    std::unique_ptr<QWidget> row(fields::plainRow({a, nullptr, b}, 4));
    auto* rowLayout = static_cast<QHBoxLayout*>(row->layout());
    CHECK(rowLayout->count() == 3);
    CHECK(rowLayout->spacing() == 4);
    CHECK(rowLayout->itemAt(1)->spacerItem() != nullptr);
    CHECK(row->property(fv::ui::prop::tileId) == QStringLiteral("actionRow"));
    std::unique_ptr<QWidget> column(fields::stack({new QLabel, new QLabel}));
    CHECK(column->layout()->spacing() == 6);
    CHECK(fields::agentHasNoApp("opencode_cli"));
    CHECK_FALSE(fields::agentHasNoApp("codex"));
}

TEST_CASE("a button row keeps its buttons at their own width on the left or right", "[widget]") {
    auto* one = new fv::Button(QStringLiteral("Cancel"));
    auto* two = new fv::Button(QStringLiteral("Save action"));
    const fields::ButtonRowBox left = fields::buttonRow({one, two});
    std::unique_ptr<QWidget> owner(left.box);
    auto* layout = static_cast<QHBoxLayout*>(left.box->layout());
    CHECK(layout->itemAt(0)->widget() == left.buttons);
    CHECK(layout->itemAt(1)->spacerItem() != nullptr);
    const int expected = std::max(one->minimumWidth(), one->sizeHint().width()) + 8 +
                         std::max(two->minimumWidth(), two->sizeHint().width());
    CHECK(left.buttons->sizeHint().width() == expected);
    two->hide();
    CHECK(left.buttons->sizeHint().width() ==
          std::max(one->minimumWidth(), one->sizeHint().width()));

    const fields::ButtonRowBox right =
        fields::buttonRow({new fv::Button(QStringLiteral("Try it"))}, fields::RowAlign::Right);
    std::unique_ptr<QWidget> rightOwner(right.box);
    CHECK(right.box->layout()->itemAt(0)->spacerItem() != nullptr);
    CHECK(right.box->layout()->itemAt(1)->widget() == right.buttons);
}

TEST_CASE("a tinted icon takes its tone from the file name and lines up the body", "[widget]") {
    fvtest::AppTheme theme;
    fv::InfoTile tile(QStringLiteral("Title"), QStringLiteral("What"));
    fields::tintIcon(&tile, QStringLiteral("terminal-green.svg"));
    CHECK(tile.iconLabel()->property(fv::ui::prop::iconTone) == QStringLiteral("green"));
    CHECK(tile.iconLabel()->size() == QSize(32, 32));
    CHECK(tile.iconLabel()->pixmap().deviceIndependentSize().toSize() == QSize(18, 18));
    tile.show();
    tile.resize(400, 100);
    QApplication::processEvents();
    CHECK(tile.bodyLayout()->contentsMargins().left() == 44);
    // Narrower than 360 px: the body gets the whole width.
    tile.resize(359, 100);
    QApplication::processEvents();
    CHECK(tile.bodyLayout()->contentsMargins().left() == 0);
    tile.resize(360, 100);
    QApplication::processEvents();
    CHECK(tile.bodyLayout()->contentsMargins().left() == 44);

    fv::InfoTile other(QStringLiteral("Title"), QStringLiteral("What"));
    fields::tintIcon(&other, QStringLiteral("copy-gray.svg"), 36, false);
    CHECK(other.iconLabel()->property(fv::ui::prop::iconTone) == QStringLiteral("gray"));
    CHECK(other.iconLabel()->size() == QSize(36, 36));
    other.show();
    other.resize(500, 100);
    QApplication::processEvents();
    CHECK(other.bodyLayout()->contentsMargins().left() == 0);
    fv::InfoTile third(QStringLiteral("Title"), QStringLiteral("What"));
    fields::tintIcon(&third, QStringLiteral("server.svg"));
    CHECK(third.iconLabel()->property(fv::ui::prop::iconTone) == QStringLiteral("blue"));
    fields::tintIcon(&third, QStringLiteral("wand-purple.svg"));
    CHECK(third.iconLabel()->property(fv::ui::prop::iconTone) == QStringLiteral("purple"));
}

TEST_CASE("columns sit side by side while each gets its minimum", "[widget]") {
    fvtest::AppTheme theme;
    fv::Columns columns(new QLabel(QStringLiteral("a")), new QLabel(QStringLiteral("b")), 240);
    columns.resize(2 * 240 + 18, 100);
    columns.show();
    QApplication::processEvents();
    CHECK_FALSE(columns.isStacked());
    columns.resize(2 * 240 + 17, 100);
    QApplication::processEvents();
    CHECK(columns.isStacked());
}

TEST_CASE("a status line shows a state, a sentence and its link", "[widget]") {
    fv::StatusLine line(QStringLiteral("Fast judgments"),
                        QStringLiteral(u"Open Settings → Meeting Mode → Fast judgments"));
    CHECK(line.objectName() == QStringLiteral("actionStatusLine"));
    CHECK(line.state() == QStringLiteral("off"));
    CHECK_FALSE(line.link()->isHidden());
    line.showState(QStringLiteral("attention"), QStringLiteral("Needs a key."), false);
    CHECK(line.property(fv::ui::prop::state) == QStringLiteral("attention"));
    CHECK(line.dot()->status() == fv::EngineStatus::Attention);
    CHECK(line.message()->text() == QStringLiteral("Needs a key."));
    CHECK(line.link()->isHidden());
    line.setLinkVisible(true);
    line.setState(QStringLiteral("ready"));
    CHECK(line.dot()->status() == fv::EngineStatus::Ready);
    CHECK_FALSE(line.link()->isHidden());
    line.setState(QStringLiteral("off"));
    CHECK(line.dot()->status() == fv::EngineStatus::Unknown);
    line.setMessage(QStringLiteral("Off."));
    CHECK(line.message()->text() == QStringLiteral("Off."));
    QSignalSpy activated(&line, &fv::StatusLine::linkActivated);
    line.link()->click();
    CHECK(activated.count() == 1);
    line.setLink(QString());
    CHECK(line.link()->isHidden());
    fv::StatusLine bare;
    CHECK(bare.link()->isHidden());
}

TEST_CASE("a path field shows the chosen path from its start", "[widget]") {
    QString askedTitle;
    QString askedStart;
    ScopedChooser chooser(fields::chooseFolder(),
                          [&](QWidget*, const QString& title, const QString& start) {
                              askedTitle = title;
                              askedStart = start;
                              return QStringLiteral("C:/Projects/./site/../app/");
                          });
    fv::PathField field(QStringLiteral("Working folder"), true,
                        QStringLiteral("Optional · where the agent starts"));
    CHECK(field.line()->isReadOnly());
    CHECK(field.line()->placeholderText() == QStringLiteral("Optional · where the agent starts"));
    CHECK(field.chooseButton()->accessibleName() == QStringLiteral("Choose working folder"));
    CHECK(field.clearButton()->accessibleName() == QStringLiteral("Clear working folder"));
    CHECK(field.clearButton()->isHidden());
    QSignalSpy changed(&field, &fv::PathField::changed);
    field.chooseButton()->click();
    CHECK(askedTitle == QStringLiteral("Working folder"));
    CHECK(askedStart.isEmpty());
    const QString expected = QDir::toNativeSeparators(QStringLiteral("C:/Projects/app"));
    CHECK(field.path() == expected);
    CHECK(field.line()->toolTip() == expected);
    CHECK(field.line()->cursorPosition() == 0);
    REQUIRE(changed.count() == 1);
    CHECK(changed.at(0).at(0).toString() == expected);
    CHECK_FALSE(field.clearButton()->isHidden());
    // The same path again is no change.
    field.chooseButton()->click();
    CHECK(changed.count() == 1);
    CHECK(askedStart == expected);
    field.clearButton()->click();
    CHECK(field.path().isEmpty());
    CHECK(changed.count() == 2);
    CHECK(field.clearButton()->isHidden());
}

TEST_CASE("a cancelled picker leaves the path", "[widget]") {
    ScopedChooser chooser(fields::chooseProgram(),
                          [](QWidget*, const QString&, const QString&) { return QString(); });
    fv::PathField field(QStringLiteral("Program"), false, QString());
    field.setPath(QStringLiteral("C:\\Tools\\log.exe"));
    QSignalSpy changed(&field, &fv::PathField::changed);
    field.chooseButton()->click();
    CHECK(changed.count() == 0);
    CHECK(field.path() == QStringLiteral("C:\\Tools\\log.exe"));
}

TEST_CASE("a shortcut field clears and restores what it showed", "[widget]") {
    fvtest::AppTheme theme;
    fv::ShortcutField field(QStringLiteral("Shortcut"));
    CHECK(field.input()->placeholderText() == QStringLiteral("Click to set a shortcut"));
    CHECK(field.input()->toolTip() ==
          QStringLiteral("Click, then press a shortcut. Escape cancels. Press it once to start, "
                         "again to send."));
    CHECK(field.input()->accessibleName() == QStringLiteral("Shortcut"));
    CHECK(field.clearButton()->accessibleName() == QStringLiteral("Clear shortcut"));
    CHECK(field.row()->maximumWidth() == 380);
    field.setHotkey(QStringLiteral("ctrl+alt+c"));
    CHECK(field.hotkey() == QStringLiteral("ctrl+alt+c"));
    CHECK(field.clearButton()->isEnabled());

    QSignalSpy captured(&field, &fv::ShortcutField::captured);
    QSignalSpy cleared(&field, &fv::ShortcutField::cleared);
    QSignalSpy capturing(&field, &fv::ShortcutField::captureChanged);
    field.clearButton()->click();
    REQUIRE(captured.count() == 1);
    CHECK(captured.at(0).at(0).toString().isEmpty());
    CHECK(cleared.count() == 1);

    // A capture remembers what was there, so a refused shortcut can be put back.
    field.show();
    field.beginCapture();
    REQUIRE(capturing.count() == 1);
    CHECK(capturing.at(0).at(0).toBool());
    field.cancelCapture();
    CHECK(capturing.count() == 2);
    field.setHotkey(QStringLiteral("f9"));
    field.restore();
    CHECK(field.hotkey() == QStringLiteral("ctrl+alt+c"));
    field.setHotkey(QString());
    CHECK_FALSE(field.clearButton()->isEnabled());

    theme.setScale(130);
    QApplication::processEvents();
    CHECK(field.row()->maximumWidth() == 494);
}

TEST_CASE("the route shortcut saves through the standard hotkey path", "[widget]") {
    fvtest::AppTheme theme;
    FakeShortcutHost host;
    host.saved = Json{{"hotkeys", Json{{"route_record", "ctrl+alt+r"}}}};
    fv::RouteShortcutField field(host);
    CHECK(field.property(fv::ui::prop::tileId) == QStringLiteral("actionRouteShortcut"));
    CHECK(field.input()->accessibleName() == QStringLiteral("Route by voice shortcut"));
    CHECK(field.input()->toolTip() ==
          QStringLiteral("Click, then press a shortcut. Escape cancels."));
    CHECK(field.clearButton()->toolTip() == QStringLiteral("Remove the Route by voice shortcut"));
    // Nothing on the Hotkeys page yet: the saved one.
    CHECK(field.hotkey() == QStringLiteral("ctrl+alt+r"));
    CHECK(field.input()->hotkey() == QStringLiteral("ctrl+alt+r"));
    CHECK(field.clearButton()->isEnabled());
    CHECK(field.message()->isHidden());

    QSignalSpy changed(&field, &fv::RouteShortcutField::changed);
    QSignalSpy captured(&field, &fv::RouteShortcutField::captured);
    Q_EMIT field.input()->captured(QStringLiteral("ctrl+shift+r"));
    REQUIRE(host.calls.size() == 1);
    CHECK(host.calls[0] ==
          std::pair{QStringLiteral("route_record"), QStringLiteral("ctrl+shift+r")});
    CHECK(changed.count() == 1);
    CHECK(captured.count() == 1);
    CHECK(field.hotkey() == QStringLiteral("ctrl+shift+r"));

    // A refusal shows why, and keeps what was saved.
    host.refusal = QStringLiteral("That shortcut is already used by Record.");
    Q_EMIT field.input()->captured(QStringLiteral("kp *"));
    CHECK(changed.count() == 1);
    CHECK_FALSE(field.message()->isHidden());
    CHECK(field.message()->text() == QStringLiteral("That shortcut is already used by Record."));
    CHECK(field.input()->hotkey() == QStringLiteral("ctrl+shift+r"));

    host.refusal.clear();
    QSignalSpy cleared(&field, &fv::RouteShortcutField::cleared);
    field.clearButton()->click();
    CHECK(cleared.count() == 1);
    CHECK(host.calls.back() == std::pair{QStringLiteral("route_record"), QString()});
    CHECK(changed.count() == 2);
    CHECK(field.message()->isHidden());
    CHECK_FALSE(field.clearButton()->isEnabled());

    // Capturing pauses the app's hotkeys.
    field.show();
    field.beginCapture();
    field.cancelCapture();
    CHECK(host.suspends == std::vector<bool>{true, false});
}

TEST_CASE("the route shortcut stacks its button when narrow", "[widget]") {
    fvtest::AppTheme theme;
    FakeShortcutHost host;
    // In a page that gives it any width, as Settings pages do.
    QWidget page;
    page.resize(500, 200);
    auto* field = new fv::RouteShortcutField(host, &page);
    field->setGeometry(0, 0, 400, 80);
    page.show();
    QApplication::processEvents();
    // Measured once styled: the theme sets the button's font.
    const int sideBySide = field->sizeHint().width();
    CHECK(sideBySide == 160 + 8 +
                            std::max(field->clearButton()->sizeHint().width(),
                                     field->clearButton()->minimumWidth()));
    field->setGeometry(0, 0, sideBySide, 80);
    QApplication::processEvents();
    CHECK(field->row()->direction() == QBoxLayout::LeftToRight);
    field->setGeometry(0, 0, sideBySide - 1, 80);
    QApplication::processEvents();
    CHECK(field->row()->direction() == QBoxLayout::TopToBottom);
    CHECK(field->input()->minimumWidth() == 160);
    field->setGeometry(0, 0, 120, 80);
    QApplication::processEvents();
    CHECK(field->input()->minimumWidth() == 120);
    // The hints don't follow the arrangement.
    CHECK(field->sizeHint().width() == sideBySide);
}

TEST_CASE("a switch row toggles from anywhere on the row", "[widget]") {
    fv::SwitchRow row(QStringLiteral("Include what I've selected"), QStringLiteral("Detail."));
    CHECK(row.objectName() == QStringLiteral("actionSwitchRow"));
    CHECK(row.switchButton()->accessibleName() == QStringLiteral("Include what I've selected"));
    CHECK(row.switchButton()->accessibleDescription() == QStringLiteral("Detail."));
    row.resize(400, 60);
    row.show();
    QApplication::processEvents();
    QTest::mouseClick(&row, Qt::LeftButton, {}, QPoint(10, 10));
    CHECK(row.switchButton()->isChecked());
    row.switchButton()->setEnabled(false);
    QTest::mouseClick(&row, Qt::LeftButton, {}, QPoint(10, 10));
    CHECK(row.switchButton()->isChecked());
}

TEST_CASE("an output combo lists where an answer goes", "[widget]") {
    fvtest::AppTheme theme;
    fv::OutputCombo combo(QStringLiteral("Reply goes to"));
    CHECK(combo.objectName() == QStringLiteral("actionOutputCombo"));
    CHECK(combo.accessibleName() == QStringLiteral("Reply goes to"));
    CHECK(combo.minimumHeight() == 36);
    REQUIRE(combo.count() == 5);
    CHECK(combo.itemText(0) == QStringLiteral("Nowhere"));
    CHECK(combo.itemData(1).toString() == QStringLiteral("paste"));
    CHECK(combo.output() == QStringLiteral("none"));
    combo.setOutput(QStringLiteral("clipboard"));
    CHECK(combo.output() == QStringLiteral("clipboard"));
    combo.setOutput(QStringLiteral("moon"));
    CHECK(combo.output() == QStringLiteral("none"));
    CHECK(combo.minimumSizeHint().width() <= fv::OutputCombo::kMinimumWidth);
    CHECK(combo.maximumWidth() == 340);
}
