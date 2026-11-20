// Tests for the button family: idle Start/Stop/Cancel visual inactive state, the width rules,
// the compact helpers and the hotkey hover hint.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>

#include <QApplication>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QTest>
#include <QWidget>

#include "ui_theme/style_props.h"
#include "ui_theme/theme_manager.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/collapse_animation.h"

namespace {

QColor fill(QWidget& button) {
    return button.grab().toImage().pixelColor(button.width() / 2, 6);
}

void sendEnter(QWidget& widget) {
    QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), QPointF(5, 5));
    QApplication::sendEvent(&widget, &enter);
}

void sendLeave(QWidget& widget) {
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&widget, &leave);
}

} // namespace

// from
// tests/test_inactive_buttons.py::TestInactiveButtons::test_set_active_publishes_inactive_property
TEST_CASE("set active publishes inactive property", "[test_inactive_buttons][widget]") {
    fv::DangerButton stop(QStringLiteral("Stop"));
    fv::WarningButton cancel(QStringLiteral("Cancel"));
    fv::SuccessButton start(QStringLiteral("Start Recording"));

    stop.setActive(false);
    cancel.setActive(false);
    start.setActive(true);

    CHECK(stop.property(fv::ui::prop::inactive).toBool());
    CHECK(cancel.property(fv::ui::prop::inactive).toBool());
    CHECK_FALSE(start.property(fv::ui::prop::inactive).toBool());

    start.setActive(false);
    CHECK(start.property(fv::ui::prop::inactive).toBool());
}

// from
// tests/test_inactive_buttons.py::TestInactiveButtons::test_idle_stop_and_cancel_drop_their_colour
TEST_CASE("idle stop and cancel drop their colour", "[test_inactive_buttons][widget]") {
    // A 28% red/orange fill still read as live; idle is a neutral outline.
    const QString previous = qApp->styleSheet();
    const fv::ui::ThemeManager manager;
    qApp->setStyleSheet(manager.stylesheet());

    QWidget host;
    auto* row = new QHBoxLayout(&host);
    fv::DangerButton stop(QStringLiteral("Stop"));
    fv::WarningButton cancel(QStringLiteral("Cancel"));
    QWidget* buttons[] = {&stop, &cancel};
    for (QWidget* button : buttons) {
        row->addWidget(button);
    }
    host.show();
    QApplication::processEvents();

    QColor live[2];
    for (int i = 0; i < 2; ++i) {
        live[i] = fill(*buttons[i]);
    }
    stop.setActive(false);
    cancel.setActive(false);
    QApplication::processEvents();
    for (int i = 0; i < 2; ++i) {
        const QColor idle = fill(*buttons[i]);
        CHECK(live[i].red() - live[i].blue() > 100);
        CHECK(std::abs(idle.red() - idle.blue()) < 24);
    }
    host.close();
    qApp->setStyleSheet(previous);
}

TEST_CASE("an inactive button stays enabled, swallows the mouse and keys, and shows the arrow",
          "[widget]") {
    fv::Button button(QStringLiteral("Start"));
    button.show();
    QApplication::processEvents();
    QSignalSpy clicked(&button, &QPushButton::clicked);
    CHECK(button.cursor().shape() == Qt::PointingHandCursor);

    button.setActive(false);
    CHECK(button.isEnabled());
    CHECK_FALSE(button.isActive());
    CHECK(button.cursor().shape() == Qt::ArrowCursor);
    QTest::mouseClick(&button, Qt::LeftButton);
    for (const Qt::Key key : {Qt::Key_Space, Qt::Key_Return, Qt::Key_Enter}) {
        button.setFocus();
        QTest::keyClick(&button, key);
    }
    CHECK(clicked.count() == 0);

    button.setActive(true);
    CHECK(button.cursor().shape() == Qt::PointingHandCursor);
    CHECK_FALSE(button.property(fv::ui::prop::inactive).toBool());
    QTest::mouseClick(&button, Qt::LeftButton);
    CHECK(clicked.count() == 1);
}

TEST_CASE("the colour buttons carry the object names the theme styles", "[widget]") {
    CHECK(fv::PrimaryButton().objectName() == QStringLiteral("primaryButton"));
    CHECK(fv::DangerButton().objectName() == QStringLiteral("dangerButton"));
    CHECK(fv::SuccessButton().objectName() == QStringLiteral("successButton"));
    CHECK(fv::WarningButton().objectName() == QStringLiteral("warningButton"));
    CHECK(fv::Button().objectName().isEmpty());
    fv::PrimaryButton primary;
    CHECK(primary.minimumHeight() == 48);
    CHECK(primary.minimumWidth() == 140);
    fv::Button plain;
    CHECK(plain.minimumHeight() == 44);
    CHECK(plain.font().pointSize() == 12);
    CHECK(plain.focusPolicy() == Qt::StrongFocus);
    CHECK(plain.sizePolicy().horizontalPolicy() == QSizePolicy::Minimum);
}

TEST_CASE("the button width is the label plus 40 px, never under 140", "[widget]") {
    fv::Button button;
    button.setText(QStringLiteral("OK"));
    CHECK(button.minimumWidth() == 140);

    const QString longText =
        QStringLiteral("A button label long enough to need more than 140 pixels");
    button.setText(longText);
    button.ensurePolished();
    const int expected =
        std::max(button.fontMetrics().horizontalAdvance(longText) + 40, button.sizeHint().width());
    CHECK(button.minimumWidth() == expected);
    CHECK(button.minimumWidth() > 140);
    CHECK(button.minimumHeight() == 44);

    // An empty label counts as zero text width.
    button.setText(QString());
    CHECK(button.minimumWidth() >= 140);
}

TEST_CASE("the base minimum size follows setBaseMinimumSize", "[widget]") {
    fv::Button button(QStringLiteral("Go"));
    button.setBaseMinimumSize(0, 34);
    CHECK(button.minimumHeight() == 34);
    CHECK(button.minimumWidth() >= button.sizeHint().width());
    CHECK(button.minimumWidth() < 140);
    button.setText(QStringLiteral("Go on"));
    CHECK(button.minimumHeight() == 34);
}

TEST_CASE("the compact helpers set the tone and the compact size", "[widget]") {
    fv::Button neutral(QStringLiteral("Browse"));
    fv::neutralButton(&neutral);
    CHECK(neutral.property(fv::ui::prop::tone).toString() == QStringLiteral("neutral"));
    CHECK(neutral.minimumHeight() == 34);

    fv::Button primary(QStringLiteral("Save"));
    CHECK(fv::compactPrimaryButton(&primary) == &primary);
    CHECK(primary.property(fv::ui::prop::tone).toString() == QStringLiteral("primary"));
    CHECK(primary.minimumHeight() == 34);

    fv::Button danger(QStringLiteral("Delete"));
    fv::compactDangerButton(&danger);
    CHECK(danger.property(fv::ui::prop::tone).toString() == QStringLiteral("danger"));
    CHECK(danger.minimumHeight() == 34);
}

TEST_CASE("fitCompactButton floors the height at 34 and fixes or frees the width", "[widget]") {
    fv::Button leaveWidth(QStringLiteral("Cancel"));
    // A plain Button is only sized when its text is set through Button::setText.
    CHECK(leaveWidth.minimumWidth() == 0);
    fv::fitCompactButton(&leaveWidth);
    CHECK(leaveWidth.minimumHeight() == leaveWidth.maximumHeight());
    CHECK(leaveWidth.minimumHeight() >= 34);
    CHECK(leaveWidth.minimumHeight() == std::max(34, leaveWidth.sizeHint().height()));
    // Its width is left alone, so only the label's own floor applies (base minimum width 0).
    leaveWidth.ensurePolished();
    CHECK(leaveWidth.minimumWidth() ==
          std::max(leaveWidth.fontMetrics().horizontalAdvance(QStringLiteral("Cancel")) + 40,
                   leaveWidth.sizeHint().width()));
    CHECK(leaveWidth.maximumWidth() == fv::kUnlimitedHeight);

    fv::Button fixed(QStringLiteral("Cancel"));
    fv::fitCompactButton(&fixed, 200);
    CHECK(fixed.minimumWidth() == fixed.maximumWidth());
    CHECK(fixed.minimumWidth() >= 200);

    fv::Button free(QStringLiteral("Cancel"));
    fv::fitCompactButton(&free, 0);
    CHECK(free.minimumWidth() == 0);
    CHECK(free.maximumWidth() == fv::kUnlimitedHeight);

    fv::Button narrow(QStringLiteral("A button label that cannot fit in 50 px"));
    fv::fitCompactButton(&narrow, 50);
    CHECK(narrow.minimumWidth() >= narrow.sizeHint().width());
}

TEST_CASE("fitStartWidth caps at the Start width and lets a narrow column shrink it", "[widget]") {
    QWidget widget;
    CHECK(fv::fitStartWidth(&widget) == &widget);
    CHECK(widget.maximumWidth() == fv::kStartButtonWidth);
    CHECK(fv::kStartButtonWidth == 340);
    CHECK(widget.sizePolicy().horizontalPolicy() == QSizePolicy::Expanding);
}

// New: HotkeyHintFilter
TEST_CASE("the hint shows on Enter only with a hotkey and an enabled button, and hides 80 ms "
          "after Leave",
          "[widget]") {
    QWidget window;
    auto* row = new QHBoxLayout(&window);
    auto* button = new QPushButton(QStringLiteral("Record"));
    row->addWidget(button);
    auto* filter = new fv::HotkeyHintFilter(button);
    CHECK(filter->parent() == button);
    window.show();
    QApplication::processEvents();

    // No hotkey: nothing to show.
    sendEnter(*button);
    CHECK(filter->hint() == nullptr);

    filter->setHotkey(QStringLiteral("Ctrl+Shift+R"));
    button->setEnabled(false);
    sendEnter(*button);
    CHECK(filter->hint() == nullptr);

    button->setEnabled(true);
    sendEnter(*button);
    REQUIRE(filter->hint() != nullptr);
    CHECK(filter->hint()->isVisible());
    CHECK(filter->hint()->objectName() == QStringLiteral("hotkeyHoverHint"));
    CHECK(filter->hint()->parentWidget() == &window);
    // The pill sits above the button, centred on it, 8 px clear.
    const QPoint top = button->mapToGlobal(QPoint(button->rect().center().x(), 0));
    CHECK(filter->hint()->y() == top.y() - filter->hint()->sizeHint().height() - 8);
    CHECK(filter->hint()->x() == top.x() - filter->hint()->sizeHint().width() / 2);

    sendLeave(*button);
    CHECK(filter->hint()->isVisible());
    QTest::qWait(fv::HotkeyHintFilter::kHideDelayMs + 120);
    CHECK_FALSE(filter->hint()->isVisible());
}

TEST_CASE("coming back before the delay keeps the hint, and hiding the button hides it at once",
          "[widget]") {
    QWidget window;
    auto* row = new QHBoxLayout(&window);
    auto* button = new QPushButton(QStringLiteral("Record"));
    row->addWidget(button);
    auto* filter = new fv::HotkeyHintFilter(button, QStringLiteral("F9"));
    window.show();
    QApplication::processEvents();

    sendEnter(*button);
    REQUIRE(filter->hint() != nullptr);
    sendLeave(*button);
    sendEnter(*button);
    QTest::qWait(fv::HotkeyHintFilter::kHideDelayMs + 120);
    CHECK(filter->hint()->isVisible());

    button->hide();
    CHECK_FALSE(filter->hint()->isVisible());
}

TEST_CASE("an inactive Button still shows its hint, and going inactive hides it", "[widget]") {
    QWidget window;
    auto* row = new QHBoxLayout(&window);
    auto* button = new fv::Button(QStringLiteral("Start"));
    row->addWidget(button);
    button->setHotkey(QStringLiteral("F9"));
    window.show();
    QApplication::processEvents();

    button->setActive(false);
    sendEnter(*button);
    fv::HotkeyHoverHint* hint = button->hotkeyHintFilter()->hint();
    REQUIRE(hint != nullptr);
    CHECK(hint->isVisible());

    button->setActive(true);
    sendEnter(*button);
    REQUIRE(hint->isVisible());
    button->setActive(false);
    CHECK_FALSE(hint->isVisible());
}

TEST_CASE("setHotkey on a visible hint retypes it", "[widget]") {
    QWidget window;
    auto* row = new QHBoxLayout(&window);
    auto* button = new QPushButton(QStringLiteral("Record"));
    row->addWidget(button);
    auto* filter = new fv::HotkeyHintFilter(button, QStringLiteral("F9"));
    window.show();
    sendEnter(*button);
    REQUIRE(filter->hint() != nullptr);
    const int before = filter->hint()->width();
    filter->setHotkey(QStringLiteral("Ctrl+Alt+Shift+F12"));
    CHECK(filter->hint()->width() > before);
}
