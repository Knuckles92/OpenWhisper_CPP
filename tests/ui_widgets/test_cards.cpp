// Card, ControlPanel, HeaderCard and the collapsible section toggle.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

#include "ui_widgets/cards.h"
#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/collapsible_header.h"

namespace {

// A collapsible card with a body of three labels, shown.
struct Shown {
    Shown() : card(QStringLiteral("Transcription"), true) {
        for (const char* text : {"one", "two", "three"}) {
            card.addContentWidget(new QLabel(QString::fromLatin1(text)));
        }
        card.resize(420, 300);
        card.show();
        QApplication::processEvents();
    }
    fv::HeaderCard card;
};

QPropertyAnimation* animationOf(fv::HeaderCard& card) {
    return card.contentWidget()->findChild<QPropertyAnimation*>();
}

} // namespace

TEST_CASE("a card has the card look's name, margins, spacing and floor", "[widget]") {
    fv::Card card;
    CHECK(card.objectName() == QStringLiteral("card"));
    CHECK(card.minimumHeight() == 100);
    CHECK(card.layout() == card.cardLayout());
    CHECK(card.cardLayout()->contentsMargins() == QMargins(16, 16, 16, 16));
    CHECK(card.cardLayout()->spacing() == 12);

    fv::ControlPanel panel;
    CHECK(panel.objectName() == QStringLiteral("controlPanel"));
    CHECK(panel.panelLayout()->contentsMargins() == QMargins(16, 16, 16, 16));
    CHECK(panel.panelLayout()->spacing() == 12);
}

TEST_CASE("a plain header card shows a bold centred title", "[widget]") {
    fv::HeaderCard card(QStringLiteral("Settings"));
    REQUIRE(card.titleLabel() != nullptr);
    CHECK(card.sectionToggle() == nullptr);
    CHECK(card.contentWidget() == nullptr);
    CHECK_FALSE(card.collapsible());
    CHECK(card.titleLabel()->objectName() == QStringLiteral("headerLabel"));
    CHECK(card.titleLabel()->text() == QStringLiteral("Settings"));
    CHECK(card.titleLabel()->alignment() == Qt::AlignCenter);
    CHECK(card.titleLabel()->font().bold());
    CHECK(card.titleLabel()->font().pointSize() == 14);
    CHECK(card.minimumHeight() == 100);
    // Header row, then a 12 px spacer, then whatever is added.
    CHECK(card.cardLayout()->count() == 2);

    auto* body = new QLabel(QStringLiteral("body"));
    card.addContentWidget(body);
    CHECK(card.cardLayout()->indexOf(body) == 2);

    card.setTitle(QStringLiteral("Other"));
    CHECK(card.titleLabel()->text() == QStringLiteral("Other"));

    // Collapsing means nothing to a card that cannot.
    QSignalSpy toggled(&card, &fv::HeaderCard::toggled);
    card.setCollapsed(true);
    CHECK_FALSE(card.isCollapsed());
    CHECK(toggled.count() == 0);
}

TEST_CASE("a collapsible card keeps its body in a content widget under a toggle", "[widget]") {
    fv::HeaderCard card(QStringLiteral("Transcription"), true);
    REQUIRE(card.sectionToggle() != nullptr);
    REQUIRE(card.contentWidget() != nullptr);
    CHECK(card.titleLabel() == nullptr);
    CHECK(card.collapsible());
    CHECK(card.minimumHeight() == 0);
    CHECK(card.sectionToggle()->text() == QStringLiteral(u"Transcription  ▸"));
    CHECK(card.sectionToggle()->toolTip() == QStringLiteral("Show transcription output"));

    auto* body = new QLabel(QStringLiteral("body"));
    card.addContentWidget(body);
    CHECK(card.contentLayout()->indexOf(body) == 0);
    CHECK(body->parentWidget() == card.contentWidget());

    card.setTitle(QStringLiteral("Output"));
    CHECK(card.sectionToggle()->text() == QStringLiteral(u"Output  ▸"));
}

TEST_CASE("HeaderCard collapse applies instantly without emit", "[widget]") {
    Shown shown;
    fv::HeaderCard& card = shown.card;
    const int expandedHeight = card.sizeHint().height();
    QSignalSpy toggled(&card, &fv::HeaderCard::toggled);

    card.setCollapsed(true, false);
    CHECK(toggled.count() == 0);
    CHECK(card.isCollapsed());
    CHECK(card.contentWidget()->isHidden());
    CHECK(card.contentWidget()->maximumHeight() == fv::kUnlimitedHeight);
    CHECK(card.contentHeight() > 0);
    CHECK_FALSE(card.sectionToggle()->isExpanded());
    // The card shrinks to its header.
    CHECK(card.minimumHeight() == 0);
    CHECK(card.maximumHeight() < expandedHeight);
    CHECK(card.maximumHeight() == card.sizeHint().height());
    const auto* animation = animationOf(card);
    CHECK((animation == nullptr || animation->state() == QAbstractAnimation::Stopped));

    card.setCollapsed(false, false);
    CHECK(toggled.count() == 0);
    CHECK_FALSE(card.isCollapsed());
    CHECK(card.contentWidget()->isVisibleTo(&card));
    CHECK(card.maximumHeight() == fv::kUnlimitedHeight);
    CHECK(card.minimumHeight() == 0);
    CHECK(card.sectionToggle()->isExpanded());
}

TEST_CASE("HeaderCard collapse with emit animates for 300 ms", "[widget]") {
    Shown shown;
    fv::HeaderCard& card = shown.card;
    QSignalSpy toggled(&card, &fv::HeaderCard::toggled);

    card.setCollapsed(true);
    REQUIRE(toggled.count() == 1);
    CHECK(toggled.takeFirst().at(0).toBool());
    QPropertyAnimation* animation = animationOf(card);
    REQUIRE(animation != nullptr);
    CHECK(animation->duration() == 300);
    CHECK(animation->easingCurve().type() == QEasingCurve::InOutCubic);
    CHECK(animation->propertyName() == QByteArray("maximumHeight"));
    CHECK(animation->state() == QAbstractAnimation::Running);
    CHECK(animation->endValue().toInt() == 0);
    // Still on screen while it runs.
    CHECK(card.contentWidget()->isVisibleTo(&card));

    QSignalSpy finished(animation, &QAbstractAnimation::finished);
    REQUIRE((finished.count() > 0 || finished.wait(3000)));
    QApplication::processEvents();
    CHECK(card.contentWidget()->isHidden());
    CHECK(card.contentWidget()->maximumHeight() == fv::kUnlimitedHeight);
    CHECK(card.maximumHeight() == card.sizeHint().height());

    card.setCollapsed(false);
    REQUIRE(toggled.count() == 1);
    CHECK_FALSE(toggled.takeFirst().at(0).toBool());
    CHECK(animation->endValue().toInt() > 0);
    QSignalSpy expanded(animation, &QAbstractAnimation::finished);
    REQUIRE((expanded.count() > 0 || expanded.wait(3000)));
    QApplication::processEvents();
    CHECK(card.contentWidget()->isVisibleTo(&card));
    CHECK(card.maximumHeight() == fv::kUnlimitedHeight);
}

TEST_CASE("an instant collapse stops an animation in flight", "[widget]") {
    Shown shown;
    fv::HeaderCard& card = shown.card;
    card.setCollapsed(true);
    QPropertyAnimation* animation = animationOf(card);
    REQUIRE(animation != nullptr);
    REQUIRE(animation->state() == QAbstractAnimation::Running);
    card.setCollapsed(false, false);
    CHECK(animation->state() == QAbstractAnimation::Stopped);
    CHECK(card.contentWidget()->maximumHeight() == fv::kUnlimitedHeight);
}

TEST_CASE("setting the state it already has changes nothing", "[widget]") {
    Shown shown;
    QSignalSpy toggled(&shown.card, &fv::HeaderCard::toggled);
    shown.card.setCollapsed(false);
    CHECK(toggled.count() == 0);
    shown.card.setCollapsed(true, false);
    shown.card.setCollapsed(true);
    CHECK(toggled.count() == 0);
}

TEST_CASE("clicking the toggle collapses and expands the card", "[widget]") {
    Shown shown;
    fv::HeaderCard& card = shown.card;
    QSignalSpy toggled(&card, &fv::HeaderCard::toggled);

    // The toggle starts as if collapsed (its arrow ▸) over an expanded card, as in OpenWhisper:
    // the first click expands, which the card, already expanded, has nothing to do for.
    QTest::mouseClick(card.sectionToggle(), Qt::LeftButton);
    CHECK(card.sectionToggle()->isExpanded());
    CHECK_FALSE(card.isCollapsed());
    CHECK(toggled.count() == 0);

    QTest::mouseClick(card.sectionToggle(), Qt::LeftButton);
    CHECK_FALSE(card.sectionToggle()->isExpanded());
    CHECK(card.isCollapsed());
    REQUIRE(toggled.count() == 1);
    CHECK(toggled.takeFirst().at(0).toBool());
}

TEST_CASE("the collapse height is the body's at the moment of collapsing", "[widget]") {
    Shown shown;
    const int body = std::max(shown.card.contentWidget()->height(),
                              shown.card.contentWidget()->sizeHint().height());
    shown.card.setCollapsed(true, false);
    CHECK(shown.card.contentHeight() == body);
}

TEST_CASE("the section toggle shows its state as an arrow and a tooltip", "[widget]") {
    fv::CollapsibleSectionToggleOptions options;
    options.prefix = QStringLiteral("> ");
    options.expandTooltip = QStringLiteral("Open");
    options.collapseTooltip = QStringLiteral("Close");
    fv::CollapsibleSectionToggle toggle(QStringLiteral("Details"), options);
    CHECK_FALSE(toggle.isExpanded());
    CHECK(toggle.text() == QStringLiteral(u"> Details  ▸"));
    CHECK(toggle.toolTip() == QStringLiteral("Open"));
    CHECK(toggle.isCheckable());
    CHECK(toggle.toolButtonStyle() == Qt::ToolButtonTextOnly);
    CHECK(toggle.cursor().shape() == Qt::PointingHandCursor);

    QSignalSpy changed(&toggle, &fv::CollapsibleSectionToggle::toggledExpanded);
    toggle.setExpanded(true);
    CHECK(toggle.isExpanded());
    CHECK(toggle.isChecked());
    CHECK(toggle.text() == QStringLiteral(u"> Details  ▾"));
    CHECK(toggle.toolTip() == QStringLiteral("Close"));
    REQUIRE(changed.count() == 1);
    CHECK(changed.takeFirst().at(0).toBool());

    // Silent, and a no-op when nothing changes.
    toggle.setExpanded(false, false);
    CHECK_FALSE(toggle.isChecked());
    toggle.setExpanded(false);
    CHECK(changed.count() == 0);

    toggle.setTitle(QStringLiteral("More"));
    CHECK(toggle.text() == QStringLiteral(u"> More  ▸"));
}

TEST_CASE("the section toggle starts expanded when asked", "[widget]") {
    fv::CollapsibleSectionToggleOptions options;
    options.expanded = true;
    fv::CollapsibleSectionToggle toggle(QStringLiteral("Details"), options);
    CHECK(toggle.isExpanded());
    CHECK(toggle.isChecked());
    CHECK(toggle.text() == QStringLiteral(u"Details  ▾"));
    CHECK(toggle.toolTip() == QStringLiteral("Hide section"));
}
