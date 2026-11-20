// Snap's logical-to-device maths, toolbar placement and the kept image
// (tests/test_snap_geometry.py), and the golden vectors of ui_qt/snap/geometry.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <set>
#include <utility>
#include <vector>

#include <QColor>
#include <QImage>

#include "golden/golden.h"
#include "snap_test_support.h"
#include "ui_snap/annotations.h"
#include "ui_snap/export.h"
#include "ui_snap/geometry.h"

using namespace fv::ui_snap;
using fvtest::coral;
using fvtest::pyRound;

namespace {

constexpr const char* kModule = "ui_qt.snap.geometry";

const QColor kWhite(QStringLiteral("#ffffff"));

QImage frozen(double dpr, QSize size = QSize(400, 300)) {
    QImage image(pyRound(size.width() * dpr), pyRound(size.height() * dpr), QImage::Format_RGB32);
    image.fill(kWhite);
    image.setDevicePixelRatio(dpr);
    return image;
}

bool isCoral(const QColor& color) {
    return color.red() > 200 && color.green() < 140 && color.blue() < 120;
}

QImage render(const QImage& image, const QRect& selection, std::vector<Annotation> items = {}) {
    return renderKept(image, selection, items);
}

} // namespace

// from tests/test_snap_geometry.py::TestDeviceRect::test_the_selection_scales_to_device_pixels
TEST_CASE("the selection scales to device pixels", "[test_snap_geometry]") {
    const auto [dpr, expected] =
        GENERATE(std::pair{1.0, QRect(10, 20, 100, 50)}, std::pair{1.5, QRect(15, 30, 150, 75)},
                 std::pair{2.0, QRect(20, 40, 200, 100)});
    const QRect imageRect(0, 0, pyRound(800 * dpr), pyRound(600 * dpr));
    CHECK(deviceRect(QRect(10, 20, 100, 50), dpr, imageRect) == expected);
}

// from tests/test_snap_geometry.py::TestDeviceRect::test_fractional_ratios_round_outward
TEST_CASE("fractional ratios round outward", "[test_snap_geometry]") {
    // 1.5..6.0 by 1.5..6.0 device pixels: keep every pixel the frame touched.
    CHECK(deviceRect(QRect(1, 1, 3, 3), 1.5, QRect(0, 0, 100, 100)) == QRect(1, 1, 5, 5));
}

// from tests/test_snap_geometry.py::TestDeviceRect::test_the_crop_never_leaves_the_image
TEST_CASE("the crop never leaves the image", "[test_snap_geometry]") {
    CHECK(deviceRect(QRect(390, 290, 50, 50), 2.0, QRect(0, 0, 800, 600)) ==
          QRect(780, 580, 20, 20));
}

// from tests/test_snap_geometry.py::test_a_drag_spans_the_same_rectangle_in_any_direction
TEST_CASE("a drag spans the same rectangle in any direction", "[test_snap_geometry]") {
    CHECK(span(QPoint(110, 60), QPoint(10, 10)) == QRect(10, 10, 100, 50));
    CHECK(span(QPoint(10, 60), QPoint(110, 10)) == QRect(10, 10, 100, 50));
}

namespace {
const QRect kBounds(0, 0, 1000, 800);
const QSize kToolbar(400, 46);
} // namespace

// from tests/test_snap_geometry.py::TestToolbarPlacement::test_centred_just_below_the_selection
TEST_CASE("the toolbar is centred just below the selection", "[test_snap_geometry]") {
    const QPoint corner = placeToolbar(kToolbar, QRect(300, 100, 400, 200), kBounds, 10);
    CHECK(corner == QPoint(300, 310));
}

// from
// tests/test_snap_geometry.py::TestToolbarPlacement::test_flips_above_when_the_selection_touches_the_bottom
TEST_CASE("the toolbar flips above when the selection touches the bottom", "[test_snap_geometry]") {
    const QRect selection(300, 500, 400, 300);
    const QPoint corner = placeToolbar(kToolbar, selection, kBounds, 10);
    CHECK(corner.y() + kToolbar.height() <= selection.top() - 10);
    CHECK(corner.y() >= kBounds.top());
}

// from
// tests/test_snap_geometry.py::TestToolbarPlacement::test_sits_inside_when_the_selection_fills_the_screen
TEST_CASE("the toolbar sits inside when the selection fills the screen", "[test_snap_geometry]") {
    const QPoint corner = placeToolbar(kToolbar, kBounds, kBounds, 10, 8);
    const QRect toolbar(corner, kToolbar);
    CHECK(kBounds.contains(toolbar));
    CHECK(toolbar.bottom() < kBounds.bottom());
}

// from tests/test_snap_geometry.py::TestToolbarPlacement::test_stays_fully_on_screen_beside_an_edge
TEST_CASE("the toolbar stays fully on screen beside an edge", "[test_snap_geometry]") {
    const QPoint corner = placeToolbar(kToolbar, QRect(0, 100, 60, 60), kBounds, kToolbarGap, 8);
    CHECK(corner.x() == 8);
    CHECK(kBounds.contains(QRect(corner, kToolbar)));
}

// from
// tests/test_snap_geometry.py::TestToolbarPlacement::test_the_size_readout_moves_inside_at_the_top_edge
TEST_CASE("the size readout moves inside at the top edge", "[test_snap_geometry]") {
    const QSize size(80, 22);
    const QRect above = badgeRect(size, QRect(100, 200, 300, 100), kBounds);
    const QRect inside = badgeRect(size, QRect(100, 2, 300, 100), kBounds);
    CHECK(above.bottom() < 200);
    CHECK(inside.top() > 2);
}

namespace {
const QRect kSelection(100, 100, 200, 100);
} // namespace

// from tests/test_snap_geometry.py::TestGrabbing::test_corners_and_edges
TEST_CASE("selection corners and edges grab", "[test_snap_geometry]") {
    const auto [point, grab] =
        GENERATE(std::pair{QPoint(100, 100), Grab{-1, -1}}, std::pair{QPoint(92, 92), Grab{-1, -1}},
                 std::pair{QPoint(300, 200), Grab{1, 1}}, std::pair{QPoint(310, 95), Grab{1, -1}},
                 // Along an edge near its end still takes the corner.
                 std::pair{QPoint(100, 112), Grab{-1, -1}}, std::pair{QPoint(200, 98), Grab{0, -1}},
                 std::pair{QPoint(303, 150), Grab{1, 0}}, std::pair{QPoint(200, 200), Grab{0, 1}});
    CAPTURE(point.x(), point.y());
    CHECK(grabAt(kSelection, point) == grab);
}

// from tests/test_snap_geometry.py::TestGrabbing::test_inside_and_far_away_grab_nothing
TEST_CASE("inside and far away grab nothing", "[test_snap_geometry]") {
    const QPoint point =
        GENERATE(QPoint(200, 150), QPoint(106, 150), QPoint(80, 80), QPoint(200, 220));
    CHECK_FALSE(grabAt(kSelection, point).has_value());
}

// from tests/test_snap_geometry.py::TestGrabbing::test_a_thin_selection_grabs_the_nearer_edge
TEST_CASE("a thin selection grabs the nearer edge", "[test_snap_geometry]") {
    const QRect thin(100, 100, 200, 6);
    CHECK(grabAt(thin, QPoint(200, 101)) == Grab{0, -1});
    CHECK(grabAt(thin, QPoint(200, 105)) == Grab{0, 1});
}

// from tests/test_snap_geometry.py::TestGrabbing::test_resizing_moves_only_the_grabbed_edges
TEST_CASE("resizing moves only the grabbed edges", "[test_snap_geometry]") {
    CHECK(resized(kSelection, Grab{1, 0}, QPoint(400, 0)) == QRect(100, 100, 300, 100));
    CHECK(resized(kSelection, Grab{-1, -1}, QPoint(50, 60)) == QRect(50, 60, 250, 140));
}

// from tests/test_snap_geometry.py::TestGrabbing::test_dragging_past_the_far_side_flips
TEST_CASE("dragging past the far side flips", "[test_snap_geometry]") {
    CHECK(resized(kSelection, Grab{-1, 0}, QPoint(350, 0)) == QRect(300, 100, 50, 100));
}

namespace {
const QRect kExportSelection(20, 10, 100, 60);
} // namespace

// from tests/test_snap_geometry.py::TestExport::test_the_crop_is_the_selection_in_device_pixels
TEST_CASE("the crop is the selection in device pixels", "[test_snap_geometry]") {
    const double dpr = GENERATE(1.0, 1.5, 2.0);
    QImage image = frozen(dpr);
    image.setPixelColor(pyRound(20 * dpr), pyRound(10 * dpr), QColor(QStringLiteral("#123456")));

    const QImage kept = render(image, kExportSelection);

    CHECK(kept.size() == QSize(pyRound(100 * dpr), pyRound(60 * dpr)));
    CHECK(kept.devicePixelRatio() == dpr);
    CHECK(kept.pixelColor(0, 0) == QColor(QStringLiteral("#123456")));
}

// from tests/test_snap_geometry.py::TestExport::test_a_box_stroke_lands_on_its_device_pixels
TEST_CASE("a box stroke lands on its device pixels", "[test_snap_geometry]") {
    const double dpr = GENERATE(1.0, 1.5, 2.0);
    const Annotation box = Annotation::box(QPointF(30, 20), QPointF(80, 50), coral());

    const QImage image = render(frozen(dpr), kExportSelection, {box});

    // The left edge runs down screen x = 30, crop x = 10: device x = 10 * dpr.
    CHECK(isCoral(image.pixelColor(pyRound(10 * dpr), pyRound(25 * dpr))));
    CHECK(isCoral(image.pixelColor(pyRound(60 * dpr), pyRound(25 * dpr))));
    // Outside the box the screenshot is untouched.
    CHECK(image.pixelColor(pyRound(85 * dpr), pyRound(50 * dpr)) == kWhite);
}

// from tests/test_snap_geometry.py::TestExport::test_an_ellipse_callout_is_drawn_at_device_scale
TEST_CASE("an ellipse callout is drawn at device scale", "[test_snap_geometry]") {
    const double dpr = GENERATE(1.0, 1.5, 2.0);
    const Annotation ellipse = Annotation::ellipse(QPointF(30, 20), QPointF(80, 50), coral());
    const QImage image = render(frozen(dpr), kExportSelection, {ellipse});
    CHECK(isCoral(image.pixelColor(pyRound(10 * dpr), pyRound(25 * dpr))));
    CHECK(image.pixelColor(pyRound(35 * dpr), pyRound(25 * dpr)) == kWhite);
}

// from tests/test_snap_geometry.py::TestExport::test_a_pen_line_is_drawn_at_device_scale
TEST_CASE("a pen line is drawn at device scale", "[test_snap_geometry]") {
    const double dpr = GENERATE(1.0, 1.5, 2.0);
    const Annotation line =
        Annotation::stroke({QPointF(30, 40), QPointF(70, 40), QPointF(110, 40)}, coral());

    const QImage image = render(frozen(dpr), kExportSelection, {line});

    CHECK(isCoral(image.pixelColor(pyRound(50 * dpr), pyRound(30 * dpr))));
    // The stroke is kPenWidth (4) logical pixels: 2 * dpr either side.
    CHECK(image.pixelColor(pyRound(50 * dpr), pyRound(30 * dpr - 4 * dpr)) == kWhite);
}

// from tests/test_snap_geometry.py::TestExport::test_marks_outside_the_selection_are_clipped
TEST_CASE("marks outside the selection are clipped", "[test_snap_geometry]") {
    const double dpr = GENERATE(1.0, 1.5, 2.0);
    const Annotation outside = Annotation::box(QPointF(150, 100), QPointF(250, 200), coral());

    const QImage image = render(frozen(dpr), kExportSelection, {outside});

    std::set<QRgb> colors;
    for (int x = 0; x < image.width(); x += 7) {
        for (int y = 0; y < image.height(); y += 7) {
            colors.insert(image.pixel(x, y));
        }
    }
    CHECK(colors == std::set<QRgb>{kWhite.rgb()});
}

// from tests/test_snap_geometry.py::test_the_whole_screen_is_the_image_at_its_logical_size
TEST_CASE("the whole screen is the image at its logical size", "[test_snap_geometry]") {
    CHECK(logicalRect(frozen(2.0, QSize(640, 400))) == QRect(0, 0, 640, 400));
}

// from tests/test_snap_geometry.py::test_redaction_replaces_all_covered_pixels_in_kept_crops
TEST_CASE("redaction replaces all covered pixels in kept crops", "[test_snap_geometry]") {
    const double dpr = GENERATE(1.0, 1.25, 1.5, 2.0);
    const QImage image = frozen(dpr);
    const QRect selection(20, 10, 100, 60);
    // Fractional coordinates and an offset crop exercise boundary coverage.
    const Annotation cover = Annotation::redaction(QPointF(80.75, 50.75), QPointF(30.25, 20.25));
    const QRect crop = deviceRect(selection, dpr, image.rect());
    const QRect covered = deviceRect(annotationRect(cover).toAlignedRect(), dpr, image.rect())
                              .translated(-crop.topLeft());

    const QImage kept = render(image, selection, {cover});

    const QColor redaction(QString::fromLatin1(kRedactionColor));
    long long wrong = 0;
    for (int x = covered.left(); x <= covered.right(); ++x) {
        for (int y = covered.top(); y <= covered.bottom(); ++y) {
            wrong += kept.pixelColor(x, y) != redaction ? 1 : 0;
        }
    }
    CHECK(wrong == 0);
    CHECK(kept.pixelColor(0, 0) == kWhite);
    // Rendering doesn't modify the original capture held by the open overlay.
    CHECK(image.pixelColor(pyRound(40 * dpr), pyRound(30 * dpr)) == kWhite);
}

TEST_CASE("span matches OpenWhisper", "[golden][test_snap_geometry]") {
    fv::golden::for_each(kModule, "span", [](const fv::golden::Case& c) {
        const QRect rect = span(fvtest::pointOf(c.args()[0]), fvtest::pointOf(c.args()[1]));
        CHECK(fv::golden::equal(c.result<fv::Json>(), fvtest::listOf(rect)));
    });
}

TEST_CASE("snap_point matches OpenWhisper", "[golden][test_snap_geometry]") {
    fv::golden::for_each(kModule, "snap_point", [](const fv::golden::Case& c) {
        std::vector<QRect> targets;
        for (const auto& target : c.args()[1]) {
            targets.push_back(fvtest::rectOf(target));
        }
        const QPointF snapped = snapPoint(fvtest::pointFOf(c.args()[0]), targets);
        CHECK(fv::golden::matches(c.result<fv::Json>(), fvtest::listOf(snapped), 1e-9));
    });
}

TEST_CASE("grab_at matches OpenWhisper", "[golden][test_snap_geometry]") {
    fv::golden::for_each(kModule, "grab_at", [](const fv::golden::Case& c) {
        const auto grab = grabAt(fvtest::rectOf(c.args()[0]), fvtest::pointOf(c.args()[1]));
        const fv::Json actual = grab ? fv::Json::array({grab->x, grab->y}) : fv::Json();
        CHECK(fv::golden::equal(c.result<fv::Json>(), actual));
    });
}

TEST_CASE("resized matches OpenWhisper", "[golden][test_snap_geometry]") {
    fv::golden::for_each(kModule, "resized", [](const fv::golden::Case& c) {
        const QPoint grab = fvtest::pointOf(c.args()[1]);
        const QRect rect = resized(fvtest::rectOf(c.args()[0]), Grab{grab.x(), grab.y()},
                                   fvtest::pointOf(c.args()[2]));
        CHECK(fv::golden::equal(c.result<fv::Json>(), fvtest::listOf(rect)));
    });
}

TEST_CASE("device_rect matches OpenWhisper", "[golden][test_snap_geometry]") {
    fv::golden::for_each(kModule, "device_rect", [](const fv::golden::Case& c) {
        const QRect rect =
            deviceRect(fvtest::rectOf(c.args()[0]), c.arg<double>(1), fvtest::rectOf(c.args()[2]));
        CHECK(fv::golden::equal(c.result<fv::Json>(), fvtest::listOf(rect)));
    });
}

TEST_CASE("place_toolbar matches OpenWhisper", "[golden][test_snap_geometry]") {
    fv::golden::for_each(kModule, "place_toolbar", [](const fv::golden::Case& c) {
        const int gap = c.arg_count() > 3 ? c.arg<int>(3) : kToolbarGap;
        const int margin = c.arg_count() > 4 ? c.arg<int>(4) : kEdgeMargin;
        const QPoint corner = placeToolbar(fvtest::sizeOf(c.args()[0]), fvtest::rectOf(c.args()[1]),
                                           fvtest::rectOf(c.args()[2]), gap, margin);
        CHECK(fv::golden::equal(c.result<fv::Json>(), fv::Json::array({corner.x(), corner.y()})));
    });
}

TEST_CASE("badge_rect matches OpenWhisper", "[golden][test_snap_geometry]") {
    fv::golden::for_each(kModule, "badge_rect", [](const fv::golden::Case& c) {
        const int gap = c.arg_count() > 3 ? c.arg<int>(3) : 6;
        const QRect rect = badgeRect(fvtest::sizeOf(c.args()[0]), fvtest::rectOf(c.args()[1]),
                                     fvtest::rectOf(c.args()[2]), gap);
        CHECK(fv::golden::equal(c.result<fv::Json>(), fvtest::listOf(rect)));
    });
}
