// ElidingComboBox (elided closed state, full-width popup) and the chevron stepper of
// NoWheelSpinBox, plus the wheel rule of the family.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QAbstractItemView>
#include <QApplication>
#include <QLineEdit>
#include <QPoint>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>

#include "ui_theme/theme_manager.h"
#include "ui_widgets/no_wheel.h"

namespace {

std::unique_ptr<fv::NoWheelSpinBox> makeBox(int minimum = 1, int maximum = 10, int value = 5) {
    auto box = std::make_unique<fv::NoWheelSpinBox>();
    box->setRange(minimum, maximum);
    box->setValue(value);
    box->resize(120, 36);
    box->show();
    QApplication::processEvents();
    return box;
}

QWheelEvent wheelUp(const QWidget& widget) {
    const QPointF position = widget.rect().center();
    return QWheelEvent(position, widget.mapToGlobal(position), QPoint(), QPoint(0, 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
}

} // namespace

// from tests/test_eliding_combo.py::test_popup_widens_to_the_longest_item_in_a_narrow_field
TEST_CASE("popup widens to the longest item in a narrow field", "[test_eliding_combo][widget]") {
    fv::ElidingComboBox combo;
    combo.addItems({QStringLiteral("gpt-transcribe"),
                    QStringLiteral("gpt-4o-mini-transcribe (retiring Feb 26, 2027)")});
    QAbstractItemView* view = combo.view();
    // The offscreen plugin's item metrics disagree with its font metrics, so measure against
    // Qt's own column hint rather than a pixel constant.
    const int widest = view->sizeHintForColumn(0);
    combo.resize(widest / 2, 36);
    combo.show();
    QApplication::processEvents();
    combo.showPopup();
    QApplication::processEvents();
    CHECK(view->minimumWidth() >= widest);
    CHECK(view->minimumWidth() > combo.width());
    combo.hidePopup();
    combo.close();
}

// from
// tests/test_no_wheel_spinbox.py::TestNoWheelSpinBox::test_hides_native_arrows_and_loads_chevrons
TEST_CASE("hides native arrows and loads chevrons", "[test_no_wheel_spinbox][widget]") {
    const auto box = makeBox();
    CHECK(box->buttonSymbols() == QAbstractSpinBox::NoButtons);
    CHECK(box->upButton()->objectName() == QStringLiteral("spinStepUp"));
    CHECK(box->downButton()->objectName() == QStringLiteral("spinStepDown"));
    CHECK_FALSE(box->upButton()->icon().isNull());
    CHECK_FALSE(box->downButton()->icon().isNull());
    CHECK(box->upButton()->toolTip() == QStringLiteral("Increase"));
    CHECK(box->downButton()->toolTip() == QStringLiteral("Decrease"));
}

// from tests/test_no_wheel_spinbox.py::TestNoWheelSpinBox::test_chevron_buttons_step_the_value
TEST_CASE("chevron buttons step the value", "[test_no_wheel_spinbox][widget]") {
    const auto box = makeBox();
    QTest::mouseClick(box->upButton(), Qt::LeftButton);
    QApplication::processEvents();
    CHECK(box->value() == 6);
    QTest::mouseClick(box->downButton(), Qt::LeftButton);
    QApplication::processEvents();
    CHECK(box->value() == 5);
}

// from tests/test_no_wheel_spinbox.py::TestNoWheelSpinBox::test_buttons_disable_at_the_range_ends
TEST_CASE("buttons disable at the range ends", "[test_no_wheel_spinbox][widget]") {
    const auto box = makeBox();
    box->setValue(box->maximum());
    QApplication::processEvents();
    CHECK_FALSE(box->upButton()->isEnabled());
    CHECK(box->downButton()->isEnabled());
    box->setValue(box->minimum());
    QApplication::processEvents();
    CHECK(box->upButton()->isEnabled());
    CHECK_FALSE(box->downButton()->isEnabled());
}

// from
// tests/test_no_wheel_spinbox.py::TestNoWheelSpinBox::test_special_value_text_fits_beside_the_stepper
TEST_CASE("special value text fits beside the stepper", "[test_no_wheel_spinbox][widget]") {
    fv::NoWheelSpinBox box;
    box.setRange(0, 65535);
    box.setSpecialValueText(QStringLiteral("Automatic"));
    box.setValue(0);
    box.adjustSize();
    box.show();
    QApplication::processEvents();
    QLineEdit* edit = box.findChild<QLineEdit*>();
    REQUIRE(edit != nullptr);
    const QMargins margins = edit->textMargins();
    const int available = edit->width() - margins.left() - margins.right();
    const int needed = box.fontMetrics().horizontalAdvance(QStringLiteral("Automatic"));
    CHECK(available >= needed);
    CHECK(box.width() >= box.sizeHint().width());
}

// from
// tests/test_no_wheel_spinbox.py::TestNoWheelSpinBox::test_stylesheet_uses_chevron_images_not_border_marks
TEST_CASE("stylesheet uses chevron images not border marks", "[test_no_wheel_spinbox][widget]") {
    const fv::ui::ThemeManager manager;
    const QString sheet = manager.stylesheet();
    CHECK(sheet.contains(QStringLiteral("chevron-up-gray.svg")));
    CHECK(sheet.contains(QStringLiteral("chevron-down-gray.svg")));
    const auto rule = [&](const QString& selector) {
        const QString after = sheet.section(selector + QStringLiteral(" {"), 1, 1);
        return after.section(QLatin1Char('}'), 0, 0);
    };
    const QString up = rule(QStringLiteral("QSpinBox::up-arrow"));
    const QString down = rule(QStringLiteral("QSpinBox::down-arrow"));
    CHECK_FALSE(up.contains(QStringLiteral("border-left")));
    CHECK_FALSE(up.contains(QStringLiteral("border-top")));
    CHECK_FALSE(down.contains(QStringLiteral("border-left")));
    CHECK_FALSE(down.contains(QStringLiteral("border-bottom")));
}

TEST_CASE("the stepper sits in the right edge and the text keeps clear of it", "[widget]") {
    const auto box = makeBox();
    const QWidget* stepper = box->upButton()->parentWidget();
    REQUIRE(stepper != nullptr);
    CHECK(stepper->objectName() == QStringLiteral("spinStepper"));
    CHECK(stepper->geometry() == QRect(box->width() - 22 - 1, 1, 22, box->height() - 2));
    CHECK(box->findChild<QLineEdit*>()->textMargins().right() == 26);
    box->resize(200, 40);
    QApplication::processEvents();
    CHECK(stepper->geometry() == QRect(200 - 22 - 1, 1, 22, 40 - 2));
}

TEST_CASE("the stepper buttons repeat after 400 ms every 80 ms and never take focus", "[widget]") {
    const auto box = makeBox();
    for (QToolButton* button : {box->upButton(), box->downButton()}) {
        CHECK(button->autoRepeat());
        CHECK(button->autoRepeatDelay() == 400);
        CHECK(button->autoRepeatInterval() == 80);
        CHECK(button->focusPolicy() == Qt::NoFocus);
        CHECK(button->iconSize() == QSize(14, 14));
    }
}

TEST_CASE("the size hints reserve the stepper", "[widget]") {
    fv::NoWheelSpinBox box;
    QSpinBox plain;
    plain.setButtonSymbols(QAbstractSpinBox::NoButtons);
    CHECK(box.sizeHint().width() == plain.sizeHint().width() + 22 + 4);
    CHECK(box.minimumSizeHint().width() == plain.minimumSizeHint().width() + 22 + 4);
}

TEST_CASE("a spin box takes the wheel only with focus", "[widget]") {
    const auto box = makeBox();
    box->clearFocus();
    QWheelEvent unfocused = wheelUp(*box);
    unfocused.setAccepted(true);
    QApplication::sendEvent(box.get(), &unfocused);
    CHECK_FALSE(unfocused.isAccepted());
    CHECK(box->value() == 5);

    box->setFocus();
    REQUIRE(box->hasFocus());
    QWheelEvent focused = wheelUp(*box);
    QApplication::sendEvent(box.get(), &focused);
    CHECK(box->value() == 6);
}

TEST_CASE("a combo takes the wheel only with focus", "[widget]") {
    fv::NoWheelComboBox combo;
    combo.addItems({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")});
    combo.setCurrentIndex(1);
    combo.show();
    QApplication::processEvents();
    combo.clearFocus();
    QWheelEvent unfocused = wheelUp(combo);
    unfocused.setAccepted(true);
    QApplication::sendEvent(&combo, &unfocused);
    CHECK_FALSE(unfocused.isAccepted());
    CHECK(combo.currentIndex() == 1);

    combo.setFocus();
    REQUIRE(combo.hasFocus());
    QWheelEvent focused = wheelUp(combo);
    QApplication::sendEvent(&combo, &focused);
    CHECK(combo.currentIndex() == 0);
}

TEST_CASE("a date edit takes the wheel only with focus and pops up a calendar", "[widget]") {
    fv::NoWheelDateEdit edit;
    edit.setDate(QDate(2026, 11, 14));
    edit.show();
    QApplication::processEvents();
    CHECK(edit.calendarPopup());
    CHECK(edit.focusPolicy() == Qt::StrongFocus);
    edit.clearFocus();
    QWheelEvent unfocused = wheelUp(edit);
    unfocused.setAccepted(true);
    QApplication::sendEvent(&edit, &unfocused);
    CHECK_FALSE(unfocused.isAccepted());
    CHECK(edit.date() == QDate(2026, 11, 14));
}

TEST_CASE("an eliding combo caps its minimum width and keeps a Fixed height", "[widget]") {
    fv::ElidingComboBox combo;
    combo.addItem(QString(120, QLatin1Char('w')));
    CHECK(combo.minimumSizeHint().width() <= fv::ElidingComboBox::kMinimumWidth);
    CHECK(combo.sizeHint().width() > fv::ElidingComboBox::kMinimumWidth);
    CHECK(combo.sizePolicy().horizontalPolicy() == QSizePolicy::Preferred);
    CHECK(combo.sizePolicy().verticalPolicy() == QSizePolicy::Fixed);
}
