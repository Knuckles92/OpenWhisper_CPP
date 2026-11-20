// Contextual field help: its links, its card's place and its lifetime.
// From tests/test_field_help.py. OpenWhisper's first two tests reach the help through Quick
// Record's model field; here the caption stands on its own in a window of the same size.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFrame>
#include <QPoint>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

#include "ui_widgets/field_help.h"

namespace {

std::vector<std::pair<QString, QString>> modelLinks() {
    return {{QStringLiteral("Open the Voice model page"), QStringLiteral("ondemand")},
            {QStringLiteral("Open Downloads"), QStringLiteral("downloads")}};
}

// A window of Quick Record's test size with the caption at its top-left.
struct Host {
    Host() {
        auto* layout = new QVBoxLayout(&window);
        help = new fv::FieldHelp(QStringLiteral("Model"), QStringLiteral("Model"),
                                 QStringLiteral("Choose a model."), modelLinks());
        layout->addWidget(help);
        layout->addStretch(1);
        window.resize(700, 500);
        window.show();
        QApplication::processEvents();
    }

    QWidget window;
    fv::FieldHelp* help = nullptr;
};

// Puts the (offscreen platform's) pointer at a point for the length of a test.
struct PointerAt {
    explicit PointerAt(const QPoint& point) : previous(QCursor::pos()) { QCursor::setPos(point); }
    ~PointerAt() { QCursor::setPos(previous); }
    PointerAt(const PointerAt&) = delete;
    PointerAt& operator=(const PointerAt&) = delete;
    QPoint previous;
};

} // namespace

// from tests/test_field_help.py::test_model_help_links_emit_destinations_and_close
TEST_CASE("model help links emit destinations and close", "[test_field_help][widget]") {
    Host host;
    QSignalSpy destinations(host.help, &fv::FieldHelp::destinationRequested);
    const std::vector<QString> expected{QStringLiteral("ondemand"), QStringLiteral("downloads")};
    REQUIRE(host.help->links().size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        host.help->showHelp();
        CHECK(host.help->card()->isVisible());
        QTest::mouseClick(host.help->links()[i], Qt::LeftButton);
        REQUIRE(destinations.size() == static_cast<qsizetype>(i + 1));
        CHECK(destinations.last().at(0).toString() == expected[i]);
        CHECK_FALSE(host.help->card()->isVisible());
    }
}

// from tests/test_field_help.py::test_help_card_is_a_window_anchored_to_the_caption
TEST_CASE("help card is a window anchored to the caption", "[test_field_help][widget]") {
    Host host;
    host.help->showHelp();
    QApplication::processEvents();
    QFrame* card = host.help->card();
    CHECK(card->isWindow());
    CHECK(card->isVisible());
    CHECK(card->parentWidget() == &host.window);
    CHECK(card->objectName() == QStringLiteral("fieldHelpCard"));
    const QPoint anchor = host.help->mapToGlobal(QPoint(0, host.help->height() + 4));
    CHECK(std::abs(card->pos().x() - anchor.x()) < 24);
    CHECK(std::abs(card->pos().y() - anchor.y()) < 24);
    CHECK(card->height() >= 80);
    CHECK(card->height() <= 280);
    CHECK(card->width() >= 160);
    CHECK(host.help->accessibleName() == QStringLiteral("Help for Model"));
}

// from tests/test_field_help.py::test_popup_stays_reachable_and_escape_dismisses
TEST_CASE("popup stays reachable and escape dismisses", "[test_field_help][widget]") {
    fv::FieldHelp help(QStringLiteral("Model"), QStringLiteral("Model"),
                       QStringLiteral("Choose a model."),
                       {{QStringLiteral("Open Downloads"), QStringLiteral("downloads")}});
    help.show();
    QApplication::processEvents();
    help.showHelp();
    {
        PointerAt pointer(help.card()->mapToGlobal(QPoint(20, 20)));
        help.dismissIfOutside();
        CHECK(help.card()->isVisible());
    }
    QTest::keyClick(help.links()[0], Qt::Key_Escape);
    CHECK_FALSE(help.card()->isVisible());
    help.showHelp();
    help.hide();
    CHECK_FALSE(help.card()->isVisible());
}

TEST_CASE("the help card closes once the pointer leaves caption and card", "[widget]") {
    Host host;
    host.help->showHelp();
    REQUIRE(host.help->card()->isVisible());
    {
        // 8 px of slop around the card still counts as over it.
        const QPoint corner = host.help->card()->mapToGlobal(QPoint(-6, -6));
        PointerAt pointer(corner);
        host.help->dismissIfOutside();
        CHECK(host.help->card()->isVisible());
    }
    {
        PointerAt pointer(host.window.mapToGlobal(QPoint(650, 450)));
        host.help->dismissIfOutside();
        CHECK_FALSE(host.help->card()->isVisible());
    }
}

namespace {

void sendEnter(QWidget* widget) {
    const QPointF local(5, 5);
    QEnterEvent enter(local, local, widget->mapToGlobal(local));
    QApplication::sendEvent(widget, &enter);
}

void sendLeave(QWidget* widget) {
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(widget, &leave);
}

} // namespace

TEST_CASE("the help card opens after 450 ms of hover, and not once the pointer left", "[widget]") {
    Host host;
    QFrame* card = host.help->card();
    // Over the caption, so the dismiss check started with the card keeps it open.
    PointerAt pointer(host.help->mapToGlobal(QPoint(5, 5)));
    QElapsedTimer clock;
    clock.start();
    sendEnter(host.help);
    QTest::qWait(150);
    if (clock.elapsed() < 450) {
        CHECK_FALSE(card->isVisible());
    }
    CHECK(QTest::qWaitFor([card] { return card->isVisible(); }, 2000));
    CHECK(clock.elapsed() >= 450);
    QTest::keyClick(card, Qt::Key_Escape);
    REQUIRE_FALSE(card->isVisible());

    sendEnter(host.help);
    QTest::qWait(100);
    sendLeave(host.help);
    QTest::qWait(600);
    CHECK_FALSE(card->isVisible());
}

TEST_CASE("space opens the help card, 360 px wide at 100 % type", "[widget]") {
    Host host;
    QFrame* card = host.help->card();
    QTest::keyClick(host.help, Qt::Key_Space);
    CHECK(card->isVisible());
    CHECK(card->width() == 360);
}
