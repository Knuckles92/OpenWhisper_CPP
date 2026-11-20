// SegmentedBar, ButtonRow, Reflow, the setting tiles, StudioSplit, ScrollingBody and FlowLayout:
// the behaviour their OpenWhisper modules describe (segmented_bar.py, button_row.py, reflow.py,
// setting_tile.py, studio.py, scrolling_body.py, flow_layout.py), which no OpenWhisper test
// pins on its own.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <vector>

#include <QApplication>
#include <QDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

#include "ui_theme/style_props.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/flow_layout.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/scrolling_body.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/studio.h"
#include "ui_widgets/wrapped_label.h"

namespace {

// Row and column of widget in the grid layout of its parent.
std::pair<int, int> cellOf(QWidget* widget) {
    auto* grid = static_cast<QGridLayout*>(widget->parentWidget()->layout());
    int row = 0;
    int column = 0;
    int rowSpan = 0;
    int columnSpan = 0;
    grid->getItemPosition(grid->indexOf(widget), &row, &column, &rowSpan, &columnSpan);
    return {row, column};
}

std::unique_ptr<fv::SegmentedBar> fourChoices() {
    return std::make_unique<fv::SegmentedBar>(std::vector<std::pair<QString, QString>>{
        {QStringLiteral("Ribbon"), QStringLiteral("A glowing trace of your voice")},
        {QStringLiteral("Bars"), QStringLiteral("A scrolling level meter")},
        {QStringLiteral("Aurora"), QStringLiteral("Flowing waves")},
        {QStringLiteral("Pulse"), QStringLiteral("A ring that swells and ripples")}});
}

int widestChoice(fv::SegmentedBar& bar) {
    int cell = 0;
    for (fv::SegmentButton* button : bar.buttons()) {
        cell = std::max(cell, button->naturalWidth());
    }
    return cell;
}

} // namespace

TEST_CASE("a segmented bar wraps four choices two and two", "[widget]") {
    auto bar = fourChoices();
    const int cell = widestChoice(*bar);
    // Room for three per row (room = width - 2 * 4 + 4), balanced to 2 + 2.
    bar->resize(3 * (cell + 4) + 4, 200);
    bar->show();
    QApplication::processEvents();
    CHECK(bar->columns() == 2);
    const auto& buttons = bar->buttons();
    CHECK(cellOf(buttons[0]) == std::pair{0, 0});
    CHECK(cellOf(buttons[1]) == std::pair{0, 1});
    CHECK(cellOf(buttons[2]) == std::pair{1, 0});
    CHECK(cellOf(buttons[3]) == std::pair{1, 1});
    for (fv::SegmentButton* button : buttons) {
        CHECK_FALSE(button->isWrapping());
        CHECK(button->minimumWidth() == cell);
    }

    bar->resize(4 * (cell + 4) + 4, 200);
    QApplication::processEvents();
    CHECK(bar->columns() == 4);
}

TEST_CASE("a segmented bar too narrow for one choice wraps the text in one column", "[widget]") {
    // In a column, as on a page: a window would hold itself as wide as its layout's minimum.
    QWidget host;
    auto* bar = fourChoices().release();
    (new QVBoxLayout(&host))->addWidget(bar);
    const int cell = widestChoice(*bar);
    host.resize(cell / 2, 400);
    host.show();
    QApplication::processEvents();
    CHECK(bar->columns() == 1);
    for (fv::SegmentButton* button : bar->buttons()) {
        CHECK(button->isWrapping());
        CHECK(button->minimumWidth() == 0);
        CHECK(button->titleLabel()->wordWrap());
        CHECK(button->detailLabel()->wordWrap());
    }
}

TEST_CASE("only a click activates a segmented bar choice", "[widget]") {
    auto bar = fourChoices();
    bar->resize(600, 120);
    bar->show();
    QApplication::processEvents();
    QSignalSpy activated(bar.get(), &fv::SegmentedBar::activated);
    QSignalSpy changed(bar.get(), &fv::SegmentedBar::currentIndexChanged);
    CHECK(bar->currentIndex() == 0);
    CHECK(bar->buttons()[0]->isChecked());

    bar->setCurrentIndex(2);
    CHECK(bar->currentIndex() == 2);
    CHECK(bar->buttons()[2]->isChecked());
    CHECK_FALSE(bar->buttons()[0]->isChecked());
    CHECK(activated.isEmpty());
    REQUIRE(changed.size() == 1);
    CHECK(changed.at(0).at(0).toInt() == 2);

    // The same index or one out of range changes nothing.
    bar->setCurrentIndex(2);
    bar->setCurrentIndex(7);
    bar->setCurrentIndex(-1);
    CHECK(changed.size() == 1);

    QTest::mouseClick(bar->buttons()[1], Qt::LeftButton);
    REQUIRE(activated.size() == 1);
    CHECK(activated.at(0).at(0).toInt() == 1);
    CHECK(changed.size() == 2);
    CHECK(bar->currentIndex() == 1);
    // The selected property sits on the first label, for the theme.
    CHECK(bar->buttons()[1]->titleLabel()->property(fv::ui::prop::selected).toBool());
    CHECK_FALSE(bar->buttons()[2]->titleLabel()->property(fv::ui::prop::selected).toBool());
}

TEST_CASE("a segmented choice gains a detail line and reports it", "[widget]") {
    fv::SegmentedBar bar({{QStringLiteral("Local"), QString()}, {QStringLiteral("Remote"), {}}},
                         true);
    CHECK(bar.buttons()[0]->detailLabel() == nullptr);
    bar.setDetail(0, QStringLiteral("This computer"));
    REQUIRE(bar.buttons()[0]->detailLabel() != nullptr);
    CHECK(bar.buttons()[0]->detailLabel()->text() == QStringLiteral("This computer"));
    CHECK(bar.buttons()[0]->accessibleDescription() == QStringLiteral("This computer"));
    bar.setTitle(1, QStringLiteral("Remote (2)"));
    CHECK(bar.buttons()[1]->titleLabel()->text() == QStringLiteral("Remote (2)"));
    CHECK(bar.buttons()[1]->accessibleName() == QStringLiteral("Remote (2)"));
    // Compact choices are at least 28 px tall, the others 36.
    CHECK(bar.buttons()[0]->sizeHint().height() >= 28);
    CHECK(bar.objectName() == QStringLiteral("mcpChoiceBar"));
    CHECK(bar.buttons()[0]->objectName() == QStringLiteral("mcpChoice"));
}

TEST_CASE("a button row wraps three buttons one over two and keeps their labels", "[widget]") {
    auto* open = new fv::Button(QStringLiteral("Open model folder"));
    auto* copy = new fv::Button(QStringLiteral("Copy path"));
    auto* details = new fv::Button(QStringLiteral("Show details"));
    fv::ButtonRow row({open, copy, details});
    CHECK(open->parentWidget() == &row);
    const int floor =
        std::max({open->minimumWidth(), copy->minimumWidth(), details->minimumWidth()});
    CHECK(row.minimumWidth() == floor);
    // Too narrow for all three, wide enough for the last two side by side.
    const int pair = copy->minimumWidth() + 8 + details->minimumWidth();
    REQUIRE(pair < open->minimumWidth() + 8 + pair);
    row.resize(std::max(pair, floor) + 4, 80);
    row.show();
    QApplication::processEvents();
    CHECK(cellOf(open) == std::pair{0, 0});
    CHECK(cellOf(copy) == std::pair{1, 0});
    CHECK(cellOf(details) == std::pair{1, 1});
    for (QPushButton* button : {static_cast<QPushButton*>(open), static_cast<QPushButton*>(copy),
                                static_cast<QPushButton*>(details)}) {
        CHECK(button->width() >= button->sizeHint().width());
    }
    // The row of one spans the row of two.
    CHECK(open->width() > copy->width());

    // Wide enough again: one row.
    row.resize(open->minimumWidth() + copy->minimumWidth() + details->minimumWidth() + 16, 80);
    QApplication::processEvents();
    CHECK(cellOf(details).first == 0);

    // A hidden button leaves the rows on the next pass.
    details->hide();
    QTest::qWait(20);
    CHECK(row.minimumWidth() == std::max(open->minimumWidth(), copy->minimumWidth()));
}

TEST_CASE("a chip button row keeps natural widths from the left", "[widget]") {
    auto* first = new QPushButton(QStringLiteral("Formal"));
    auto* second = new QPushButton(QStringLiteral("Casual"));
    fv::ButtonRow row({first, second}, false);
    row.resize(400, 40);
    row.show();
    QApplication::processEvents();
    CHECK(cellOf(first) == std::pair{0, 0});
    CHECK(cellOf(second) == std::pair{0, 1});
    CHECK(first->x() == 0);
    CHECK(first->width() == first->sizeHint().width());
    CHECK(second->width() == second->sizeHint().width());
}

TEST_CASE("a reflow stacks its actions below lead plus 8 plus the actions", "[widget]") {
    // In a column, as on a page: a window would hold itself as wide as its layout's minimum.
    QWidget host;
    auto* lead = new QLabel(QStringLiteral("Snippets expand as you type"));
    auto* action = new fv::PrimaryButton(QStringLiteral("Add snippet"));
    auto* reflow = new fv::Reflow(lead, action);
    auto* column = new QVBoxLayout(&host);
    column->setContentsMargins(0, 0, 0, 0);
    column->addWidget(reflow);
    CHECK(reflow->objectName() == QStringLiteral("reflow"));
    host.resize(600, 80);
    host.show();
    QApplication::processEvents();
    // Measured once shown, in the font the widgets are polished with.
    const int needed = std::max(lead->sizeHint().width(), lead->minimumWidth()) + 8 +
                       std::max(action->sizeHint().width(), action->minimumWidth());
    host.resize(needed, 80);
    QApplication::processEvents();
    CHECK_FALSE(reflow->isStacked());
    CHECK(cellOf(action) == std::pair{0, 1});

    host.resize(needed - 1, 80);
    QApplication::processEvents();
    CHECK(reflow->isStacked());
    CHECK(cellOf(action) == std::pair{1, 0});
    // Stacked actions sit at the right by default.
    CHECK(action->geometry().right() == reflow->width() - 1);
}

TEST_CASE("a reflow with a lead width measures it at the font scale", "[widget]") {
    QWidget host;
    auto* lead = new QLabel(QStringLiteral("x"));
    auto* action = new QPushButton(QStringLiteral("Clear"));
    auto* reflow = new fv::Reflow(lead, action, 200, nullptr, Qt::AlignLeft);
    auto* column = new QVBoxLayout(&host);
    column->setContentsMargins(0, 0, 0, 0);
    column->addWidget(reflow);
    host.resize(600, 80);
    host.show();
    QApplication::processEvents();
    const int needed = 200 + 8 + std::max(action->sizeHint().width(), action->minimumWidth());
    host.resize(needed, 80);
    QApplication::processEvents();
    CHECK_FALSE(reflow->isStacked());
    host.resize(needed - 1, 80);
    QApplication::processEvents();
    CHECK(reflow->isStacked());
    CHECK(action->x() == 0);
}

TEST_CASE("a setting tile toggles its switch on a click and mirrors checked", "[widget]") {
    fv::SettingTile tile(QStringLiteral("Paste into the active window"),
                         QStringLiteral("Types the transcript where your cursor is."));
    tile.resize(480, 80);
    tile.show();
    QApplication::processEvents();
    CHECK(tile.objectName() == QStringLiteral("settingsTile"));
    CHECK(tile.property(fv::ui::prop::kind).toString() == QStringLiteral("toggle"));
    CHECK(tile.property(fv::ui::prop::checked).toBool() == false);
    CHECK(tile.checkbox()->objectName() == QStringLiteral("settingsTileCheck"));
    CHECK(tile.checkbox()->accessibleName() == QStringLiteral("Paste into the active window"));

    QTest::mouseClick(&tile, Qt::LeftButton, Qt::NoModifier, QPoint(200, 20));
    CHECK(tile.checkbox()->isChecked());
    CHECK(tile.property(fv::ui::prop::checked).toBool());
    QTest::mouseClick(tile.titleLabel(), Qt::LeftButton);
    CHECK_FALSE(tile.checkbox()->isChecked());
    CHECK(tile.property(fv::ui::prop::checked).toBool() == false);
    // Another button does nothing.
    QTest::mouseClick(&tile, Qt::RightButton, Qt::NoModifier, QPoint(200, 20));
    CHECK_FALSE(tile.checkbox()->isChecked());

    tile.setEnabled(false);
    CHECK(tile.cursor().shape() == Qt::ArrowCursor);
    QTest::mouseClick(&tile, Qt::LeftButton, Qt::NoModifier, QPoint(200, 20));
    CHECK_FALSE(tile.checkbox()->isChecked());
    tile.setEnabled(true);
    CHECK(tile.cursor().shape() == Qt::PointingHandCursor);
}

TEST_CASE("tiles lay out the icon, the text, the trailing slot and an indented body", "[widget]") {
    auto* combo = new QLabel(QStringLiteral("value"));
    fv::FieldTile field(QStringLiteral("Text size"), QStringLiteral("How large text is."), combo);
    CHECK(field.property(fv::ui::prop::kind).toString() == QStringLiteral("field"));
    CHECK(field.control() == combo);
    CHECK(combo->parentWidget() == field.body());
    CHECK_FALSE(field.body()->isHidden());
    CHECK(field.bodyLayout()->contentsMargins().left() == 32);
    field.setBodyIndented(false);
    CHECK(field.bodyLayout()->contentsMargins().left() == 0);

    auto* spin = new QLabel(QStringLiteral("5"));
    fv::FieldTile compact(QStringLiteral("Seconds"), QString(), spin, {}, true);
    CHECK(compact.body()->isHidden());
    CHECK(spin->parentWidget() == &compact);

    fv::InfoTile info(QStringLiteral("Microphone access is off"), QStringLiteral("Turn it on."));
    CHECK(info.property(fv::ui::prop::kind).toString() == QStringLiteral("field"));
    CHECK(info.body()->isHidden());
    CHECK(info.iconLabel()->size() == QSize(20, 20));
    auto* action = new QPushButton(QStringLiteral("Open privacy settings"));
    info.addActions({action});
    CHECK_FALSE(info.body()->isHidden());
    CHECK(action->parentWidget() == info.body());
    info.setDescription(QStringLiteral("Changed."));
    CHECK(info.descriptionLabel()->text() == QStringLiteral("Changed."));

    QLabel* badge = info.addTitleBadge(QStringLiteral("Experimental"));
    CHECK(badge->objectName() == QStringLiteral("meetingBetaBadge"));
    CHECK(badge->parentWidget() == info.titleLabel()->parentWidget());
    CHECK(info.titleLabel()->parentWidget()->objectName() == QStringLiteral("settingsTileText"));
}

TEST_CASE("a studio split stacks under column plus 16 plus the stage minimum", "[widget]") {
    auto* column = new QWidget;
    auto* stage = new fv::Stage;
    fv::StudioSplit split(column, stage);
    CHECK(split.objectName() == QStringLiteral("walkthroughStudio"));
    CHECK(stage->objectName() == QStringLiteral("walkthroughStage"));
    CHECK(stage->sizeHint() == QSize(704, 520));
    CHECK(stage->minimumSizeHint() == QSize(400, 520));
    CHECK_FALSE(split.isStacked());
    CHECK(column->minimumWidth() == 320);
    CHECK(column->maximumWidth() == 320);

    const int needed = 320 + 16 + 400;
    split.placeForWidth(needed - 1);
    CHECK(split.isStacked());
    CHECK(column->maximumWidth() == QWIDGETSIZE_MAX);
    CHECK(column->minimumWidth() == 0);
    split.placeForWidth(needed);
    CHECK_FALSE(split.isStacked());
    // A width not known yet keeps the columns side by side.
    split.placeForWidth(0);
    CHECK_FALSE(split.isStacked());

    split.setColumnWidth(280);
    split.placeForWidth(280 + 16 + 400 - 1);
    CHECK(split.isStacked());
    CHECK(split.columnWidth() == 280);
}

TEST_CASE("a stage shows one pane at a time, filling it", "[widget]") {
    fv::Stage stage(QStringLiteral("meetingStage"), 600, 300);
    CHECK(stage.sizeHint() == QSize(600, 300));
    auto* first = stage.addPane(new QWidget);
    auto* second = stage.addPane(new QWidget);
    stage.resize(500, 400);
    stage.show();
    stage.showPane(second);
    CHECK(stage.current() == second);
    CHECK(second->isVisible());
    CHECK_FALSE(first->isVisible());
    CHECK(second->geometry() == stage.rect());
    stage.resize(450, 380);
    CHECK(first->geometry() == stage.rect());

    // A pane its page deletes leaves the stage without dangling.
    delete second;
    CHECK(stage.current() == nullptr);
    stage.resize(420, 360);
    CHECK(first->geometry() == stage.rect());
    stage.showPane(first);
    CHECK(first->isVisible());
}

namespace {

struct BodyDialog {
    explicit BodyDialog(const QString& copy, int maxHeight = fv::ScrollingBody::kDefaultMaxHeight)
        : body(new fv::ScrollingBody(maxHeight)) {
        auto* layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(body);
        auto* label = new QLabel(copy);
        label->setWordWrap(true);
        body->addWidget(label);
        dialog.resize(360, 100);
    }

    QDialog dialog;
    fv::ScrollingBody* body;
};

QString lines(int count) {
    QStringList words;
    for (int i = 0; i < count; ++i) {
        words << QStringLiteral("Every line of this explanation stays readable.");
    }
    return words.join(QLatin1Char(' '));
}

} // namespace

TEST_CASE("fitDialog grows the copy to its height, capped at 420 or the screen less 160",
          "[widget]") {
    BodyDialog small(QStringLiteral("Short."));
    CHECK(small.body->objectName() == QStringLiteral("dialogScrollingBody"));
    CHECK(small.body->content()->objectName() == QStringLiteral("dialogScrollingBodyContent"));
    fv::fitDialog(&small.dialog, small.body);
    // Never below the area's minimum.
    CHECK(small.dialog.height() == 96);

    BodyDialog medium(lines(15));
    medium.body->fitDialog(&medium.dialog);
    CHECK(medium.dialog.height() > 96);
    CHECK(medium.dialog.height() < 420);

    BodyDialog large(lines(80));
    large.body->fitDialog(&large.dialog);
    CHECK(large.dialog.height() == 420);

    // Copy taller than the screen allows in the font in use, measured at the width the dialog
    // gives it: 80 lines are with Windows' metrics, and a smaller font needs more.
    const int screen = QGuiApplication::primaryScreen()->availableGeometry().height();
    const auto tallerThanScreen = [&](BodyDialog& made) {
        made.body->fitDialog(&made.dialog);
        const auto* copy = made.body->content()->findChild<QLabel*>();
        return copy->heightForWidth(made.body->width()) > screen;
    };
    int count = 80;
    while (!tallerThanScreen(*std::make_unique<BodyDialog>(lines(count), 10000))) {
        count *= 2;
    }
    BodyDialog tall(lines(count), 10000);
    tall.body->fitDialog(&tall.dialog);
    REQUIRE(tall.dialog.screen()->availableGeometry().height() == screen);
    CHECK(tall.dialog.height() == screen - 160);
}

TEST_CASE("a flow layout wraps its items and skips hidden ones", "[widget]") {
    QWidget host;
    auto* flow = new fv::FlowLayout(&host, 6);
    std::vector<QPushButton*> chips;
    for (const char* text : {"Alpha", "Beta", "Gamma", "Delta"}) {
        auto* chip = new QPushButton(QString::fromLatin1(text));
        flow->addWidget(chip);
        chips.push_back(chip);
    }
    CHECK(flow->hasHeightForWidth());
    int line = 0;
    for (QPushButton* chip : chips) {
        line += chip->sizeHint().width();
    }
    CHECK(flow->lineWidth() == line + 3 * 6);
    const int oneLine = flow->heightForWidth(flow->lineWidth());
    CHECK(flow->heightForWidth(flow->lineWidth() - 1) > oneLine);
    host.resize(flow->lineWidth(), oneLine);
    host.show();
    QApplication::processEvents();
    CHECK(chips[3]->y() == chips[0]->y());
    chips[1]->hide();
    QApplication::processEvents();
    CHECK(flow->lineWidth() == line - chips[1]->sizeHint().width() + 2 * 6);
    CHECK(chips[2]->x() == chips[0]->width() + 6);
}
