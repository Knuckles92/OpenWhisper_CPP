// The building blocks of the engine card's field row: the status dot, the combo that carries it,
// the narrow-surviving field combos, the Device labels and the captioned field column.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QComboBox>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

#include "app_theme.h"
#include "ui_theme/palette.h"
#include "ui_widgets/engine_field.h"

namespace {

// The color at the middle of the dot's grab.
QColor centre(fv::StatusDot& dot) {
    dot.show();
    QApplication::processEvents();
    const QImage image = dot.grab().toImage();
    return image.pixelColor(image.width() / 2, image.height() / 2);
}

QWidget* helpCard() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QStringLiteral("fieldHelpCard")) {
            return widget;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("the status dot paints the engine's readiness", "[widget]") {
    fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::StatusDot dot;
    CHECK(dot.objectName() == QStringLiteral("engineStatusDot"));
    CHECK(dot.size() == QSize(fv::kDotDiameter, fv::kDotDiameter));
    CHECK(dot.status() == fv::EngineStatus::Unknown);
    CHECK(centre(dot) == fv::ui::tokenColor("text-muted"));
    dot.setStatus(fv::EngineStatus::Ready);
    CHECK(centre(dot) == fv::ui::tokenColor("success"));
    dot.setStatus(fv::EngineStatus::Attention);
    CHECK(centre(dot) == fv::ui::tokenColor("warning"));
}

TEST_CASE("the status dot spins only while it is busy and shown", "[widget]") {
    fv::StatusDot dot(nullptr, 16);
    CHECK(dot.size() == QSize(16, 16));
    dot.setBusy(true);
    // Not shown yet: nothing to animate for.
    CHECK_FALSE(dot.isAnimating());
    dot.show();
    CHECK(dot.isAnimating());
    dot.hide();
    CHECK_FALSE(dot.isAnimating());
    dot.show();
    CHECK(dot.isAnimating());
    dot.setBusy(false);
    CHECK_FALSE(dot.isAnimating());
}

TEST_CASE("the field combos shrink to the engine field floor", "[widget]") {
    QComboBox* plain = fv::engineCombo({QStringLiteral("Parakeet TDT 0.6B v3 with a very long name "
                                                       "that would raise the window's minimum")});
    CHECK(plain->objectName() == QStringLiteral("engineField"));
    CHECK(plain->minimumSizeHint().width() <= fv::kFieldMinimumWidth);
    CHECK(plain->count() == 1);
    CHECK(qobject_cast<fv::StatusFieldCombo*>(plain) == nullptr);
    CHECK(qobject_cast<fv::ElidingComboBox*>(plain) != nullptr);

    auto* primary = static_cast<fv::StatusFieldCombo*>(fv::engineCombo({}, true));
    CHECK(primary->objectName() == QStringLiteral("engineFieldPrimary"));
    CHECK(qobject_cast<fv::StatusFieldCombo*>(primary) != nullptr);
    primary->addItem(QStringLiteral("A backend with a name that is much too long for a field"));
    CHECK(primary->minimumSizeHint().width() <= fv::kFieldMinimumWidth);
    delete plain;
    delete primary;
}

TEST_CASE("the combo carries its status dot in the left padding", "[widget]") {
    auto* combo =
        static_cast<fv::StatusFieldCombo*>(fv::engineCombo({QStringLiteral("Parakeet")}, true));
    combo->resize(200, 34);
    combo->show();
    QApplication::processEvents();
    CHECK(combo->dot()->parentWidget() == combo);
    CHECK(combo->dot()->pos() ==
          QPoint(fv::StatusFieldCombo::kDotLeft, (combo->height() - combo->dot()->height()) / 2));
    combo->resize(200, 40);
    QApplication::processEvents();
    CHECK(combo->dot()->y() == (40 - fv::kDotDiameter) / 2);

    // The indent is a theme property: switched off with the dot, so the value reclaims it.
    combo->setStatusVisible(false);
    CHECK(combo->dot()->isHidden());
    CHECK(combo->property("dot").toString() == QStringLiteral("false"));
    combo->setStatusVisible(true);
    CHECK_FALSE(combo->dot()->isHidden());
    CHECK(combo->property("dot").toString() == QStringLiteral("true"));

    combo->setStatus(fv::EngineStatus::Attention);
    CHECK(combo->dot()->status() == fv::EngineStatus::Attention);
    delete combo;
}

TEST_CASE("device fields show a label and keep the saved device", "[widget]") {
    CHECK(fv::deviceLabel(QStringLiteral("auto")) == QStringLiteral("Auto"));
    CHECK(fv::deviceLabel(QStringLiteral("cuda")) == QStringLiteral("GPU"));
    CHECK(fv::deviceLabel(QStringLiteral("cpu")) == QStringLiteral("CPU"));
    CHECK(fv::deviceLabel(QStringLiteral("vulkan")) == QStringLiteral("vulkan"));

    QComboBox combo;
    combo.addItem(QStringLiteral("stale"));
    fv::setDeviceChoices(&combo, {QStringLiteral("auto"), QStringLiteral("cuda"),
                                  QStringLiteral("cpu"), QStringLiteral("vulkan")});
    REQUIRE(combo.count() == 4);
    CHECK(combo.itemText(0) == QStringLiteral("Auto"));
    CHECK(combo.itemText(1) == QStringLiteral("GPU"));
    CHECK(combo.itemData(1).toString() == QStringLiteral("cuda"));
    CHECK(combo.itemText(3) == QStringLiteral("vulkan"));
    CHECK(combo.findData(QStringLiteral("cpu")) == 2);
}

TEST_CASE("a field stacks a plain caption over its widget", "[widget]") {
    auto* combo = fv::engineCombo({QStringLiteral("x")});
    QWidget* field = fv::engineField(QStringLiteral("Device"), combo);
    CHECK(field->objectName() == QStringLiteral("engineFieldGroup"));
    auto* column = qobject_cast<QVBoxLayout*>(field->layout());
    REQUIRE(column != nullptr);
    CHECK(column->spacing() == 4);
    CHECK(column->contentsMargins() == QMargins());
    REQUIRE(column->count() == 2);
    auto* caption = qobject_cast<QLabel*>(column->itemAt(0)->widget());
    REQUIRE(caption != nullptr);
    CHECK(caption->objectName() == QStringLiteral("engineFieldLabel"));
    CHECK(caption->text() == QStringLiteral("Device"));
    CHECK(column->itemAt(1)->widget() == combo);
    delete field;
}

TEST_CASE("a field with help has a caption that opens its card and follows its links", "[widget]") {
    fvtest::AppTheme theme(QStringLiteral("dark"));
    QWidget host;
    auto* layout = new QVBoxLayout(&host);
    QString destination;
    QWidget* field = fv::engineField(
        QStringLiteral("Model"), fv::engineCombo({QStringLiteral("x")}),
        QStringLiteral("The speech model turns audio into text."),
        {{QStringLiteral(u"Open Settings → Voice model"), QStringLiteral("ondemand")},
         {QStringLiteral(u"Open Settings → Downloads"), QStringLiteral("downloads")}},
        [&](const QString& where) { destination = where; });
    layout->addWidget(field);
    host.show();
    QApplication::processEvents();

    auto* caption = field->findChild<QPushButton*>();
    REQUIRE(caption != nullptr);
    CHECK(caption->accessibleName() == QStringLiteral("Help for Model"));
    CHECK(caption->text().isEmpty());
    // The card is a window of its own, hidden until the caption asks for it.
    QWidget* card = helpCard();
    REQUIRE(card != nullptr);
    CHECK_FALSE(card->isVisible());

    // Hovering opens the card after 450 ms.
    QEnterEvent enter(QPointF(1, 1), QPointF(1, 1), QPointF(1, 1));
    QApplication::sendEvent(caption, &enter);
    CHECK_FALSE(card->isVisible());
    QTest::qWait(550);
    card = helpCard();
    REQUIRE(card != nullptr);
    CHECK(card->isVisible());
    CHECK(card->findChild<QLabel*>(QStringLiteral("fieldHelpTitle"))->text() ==
          QStringLiteral("Model"));
    const auto links = card->findChildren<QPushButton*>();
    REQUIRE(links.size() == 2);
    CHECK(links[0]->text() == QStringLiteral(u"Open Settings → Voice model"));

    // Escape closes it.
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(links[0], &escape);
    CHECK_FALSE(card->isVisible());

    // A link reports its destination and closes the card.
    caption->click();
    REQUIRE(card->isVisible());
    links[1]->click();
    CHECK(destination == QStringLiteral("downloads"));
    CHECK_FALSE(card->isVisible());
}
