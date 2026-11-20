// The flying screenshot (ui_qt/widgets/screenshot_flight.py): its path against the golden cases
// of tools/parity/cases/ui_qt.widgets.screenshot_flight.py, and its window.
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <QImage>
#include <QRectF>
#include <QWidget>

#include "golden/golden.h"
#include "panel_support.h"
#include "ui_walkthrough/screenshot_flight.h"

namespace fvtest {
namespace {

QRectF rectOf(const fv::Json& values) {
    const auto v = fv::golden::decode<std::vector<double>>(values);
    return QRectF(v.at(0), v.at(1), v.at(2), v.at(3));
}

} // namespace

TEST_CASE("the card follows OpenWhisper's arc", "[golden][screenshot_flight]") {
    fv::golden::for_each("ui_qt.widgets.screenshot_flight", "card_rect",
                         [](const fv::golden::Case& c) {
                             const QRectF actual = fv::flightCardRect(
                                 c.arg<double>(0), rectOf(c.args().at(1)), rectOf(c.args().at(2)));
                             const QRectF expected = rectOf(c.raw().at("result"));
                             const double tolerance = c.float_tolerance();
                             CHECK(fv::golden::near(expected.x(), actual.x(), tolerance));
                             CHECK(fv::golden::near(expected.y(), actual.y(), tolerance));
                             CHECK(fv::golden::near(expected.width(), actual.width(), tolerance));
                             CHECK(fv::golden::near(expected.height(), actual.height(), tolerance));
                         });
    fv::golden::for_each(
        "ui_qt.widgets.screenshot_flight", "FLIGHT_S",
        [](const fv::golden::Case& c) { CHECK(c.result<double>() == fv::kFlightS); });
}

TEST_CASE("a flight is click-through and covers its arc on every frame",
          "[screenshot_flight][widget]") {
    FakeDesktopScope desktop;
    QWidget panel;
    fv::ScreenshotFlight flight(&panel);
    CHECK(flight.isWindow());
    CHECK(flight.testAttribute(Qt::WA_TranslucentBackground));
    CHECK(flight.testAttribute(Qt::WA_ShowWithoutActivating));
    CHECK_FALSE(flight.testAttribute(Qt::WA_QuitOnClose));

    const QRectF source(100, 200, 640, 360);
    const QRectF target(1500, 300, 252, 141.75);
    flight.start(3, image(), source, target);
    CHECK(flight.isVisible());
    CHECK(flight.number() == 3);
    CHECK(flight.progress() == 0.0);
    CHECK(flight.geometry() == source.united(target).adjusted(-64, -128, 64, 64).toAlignedRect());
    CHECK(flight.cardRect() == source);

    // A slot that moved (the column scrolled) moves the window with it.
    const QRectF moved(1500, 500, 252, 141.75);
    flight.advance(1.5, moved);
    CHECK(flight.progress() == 1.0);
    CHECK(flight.geometry() == source.united(moved).adjusted(-64, -128, 64, 64).toAlignedRect());
    CHECK(flight.cardRect() == moved);
    flight.grab();

    flight.land();
    CHECK_FALSE(flight.isVisible());
    CHECK(flight.number() == 0);
    CHECK(flight.image().isNull());
}

} // namespace fvtest
