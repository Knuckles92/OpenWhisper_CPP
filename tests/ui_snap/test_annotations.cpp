// Snap's annotation model: undo and redo, step numbers, drafts and shapes
// (tests/test_snap_annotations.py), and the golden vectors of ui_qt/snap/annotations.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include "golden/golden.h"
#include "snap_test_support.h"
#include "ui_snap/annotations.h"

using namespace fv::ui_snap;
using fvtest::coral;

namespace {

constexpr const char* kModule = "ui_qt.snap.annotations";
constexpr double kTolerance = 1e-6;

Annotation arrowOf(double x1, double y1, double x2, double y2) {
    return Annotation::arrow(QPointF(x1, y1), QPointF(x2, y2), coral());
}

QImage transparent(int width, int height) {
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    return image;
}

// The box around every pixel with any alpha.
QRect paintedRect(const QImage& image) {
    int left = image.width();
    int top = image.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) != 0) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

} // namespace

// from tests/test_snap_annotations.py::TestStack::test_undo_and_redo_walk_back_and_forth
TEST_CASE("undo and redo walk back and forth", "[test_snap_annotations]") {
    AnnotationStack stack;
    const Annotation first = arrowOf(0, 0, 50, 0);
    const Annotation second = arrowOf(0, 10, 50, 10);
    stack.add(first);
    stack.add(second);

    CHECK(stack.undo() == second);
    REQUIRE(stack.items().size() == 1);
    CHECK(stack.items()[0] == first);
    CHECK((stack.canUndo() && stack.canRedo()));
    CHECK(stack.redo() == second);
    REQUIRE(stack.items().size() == 2);
    CHECK(stack.items()[0] == first);
    CHECK(stack.items()[1] == second);
    CHECK_FALSE(stack.canRedo());
}

// from tests/test_snap_annotations.py::TestStack::test_a_new_mark_after_undo_forgets_the_redo
TEST_CASE("a new mark after undo forgets the redo", "[test_snap_annotations]") {
    AnnotationStack stack;
    stack.add(arrowOf(0, 0, 50, 0));
    stack.undo();
    stack.add(arrowOf(0, 10, 50, 10));

    CHECK_FALSE(stack.canRedo());
    CHECK_FALSE(stack.redo().has_value());
    CHECK(stack.size() == 1);
}

// from tests/test_snap_annotations.py::TestStack::test_undo_and_redo_with_nothing_to_do_return_none
TEST_CASE("undo and redo with nothing to do return none", "[test_snap_annotations]") {
    AnnotationStack stack;
    CHECK_FALSE(stack.undo().has_value());
    CHECK_FALSE(stack.redo().has_value());
    CHECK_FALSE(stack.canUndo());
    CHECK_FALSE(stack.canRedo());
}

// from
// tests/test_snap_annotations.py::TestStack::test_steps_count_up_and_undo_gives_the_number_back
TEST_CASE("steps count up and undo gives the number back", "[test_snap_annotations]") {
    AnnotationStack stack;
    CHECK(stack.nextStepNumber() == 1);
    stack.add(Annotation::step(QPointF(10, 10), stack.nextStepNumber(), coral()));
    stack.add(arrowOf(0, 0, 50, 0));
    stack.add(Annotation::step(QPointF(30, 10), stack.nextStepNumber(), coral()));

    std::vector<int> numbers;
    for (const Annotation& item : stack.items()) {
        if (item.kind == AnnotationKind::Step) {
            numbers.push_back(item.number);
        }
    }
    CHECK(numbers == std::vector<int>{1, 2});
    CHECK(stack.nextStepNumber() == 3);
    stack.undo();
    CHECK(stack.nextStepNumber() == 2);
    stack.redo();
    CHECK(stack.nextStepNumber() == 3);
}

// from
// tests/test_snap_annotations.py::TestDrafts::test_infill_controls_interior_without_changing_outline
TEST_CASE("infill controls interior without changing outline", "[test_snap_annotations]") {
    const SnapTool tool = GENERATE(SnapTool::Box, SnapTool::Ellipse);
    const bool infill = GENERATE(false, true);
    std::optional<Annotation> box = newDraft(tool, QPointF(20, 20), coral(), infill);
    REQUIRE(box.has_value());
    extendAnnotation(*box, QPointF(100, 80));
    QImage image = transparent(120, 100);
    {
        QPainter painter(&image);
        paintAnnotation(painter, *box);
    }

    CHECK(box->infill == infill);
    CHECK((image.pixelColor(60, 50).alpha() > 0) == infill);
    CHECK(image.pixelColor(60, 20).alpha() > 0);
}

// from tests/test_snap_annotations.py::TestDrafts::test_shapes_have_no_infill_by_default
TEST_CASE("shapes have no infill by default", "[test_snap_annotations]") {
    const SnapTool tool = GENERATE(SnapTool::Box, SnapTool::Ellipse);
    CHECK_FALSE(newDraft(tool, QPointF(20, 20), coral())->infill);
}

// from
// tests/test_snap_annotations.py::TestDrafts::test_redactions_start_as_a_drag_and_ignore_colour_and_infill
TEST_CASE("redactions start as a drag and ignore colour and infill", "[test_snap_annotations]") {
    std::optional<Annotation> cover = newDraft(SnapTool::Redact, QPointF(10, 20), coral(), false);
    REQUIRE(cover.has_value());
    CHECK(cover->kind == AnnotationKind::Redaction);
    CHECK_FALSE(meaningful(*cover));
    extendAnnotation(*cover, QPointF(50, 40));
    CHECK(meaningful(*cover));
    CHECK(annotationRect(*cover) == QRectF(10, 20, 40, 20));
}

// from tests/test_snap_annotations.py::TestDrafts::test_drag_tools_start_a_draft_at_the_press
TEST_CASE("drag tools start a draft at the press", "[test_snap_annotations]") {
    const auto [tool, kind] = GENERATE(std::pair{SnapTool::Arrow, AnnotationKind::Arrow},
                                       std::pair{SnapTool::Box, AnnotationKind::Box},
                                       std::pair{SnapTool::Ellipse, AnnotationKind::Ellipse},
                                       std::pair{SnapTool::Pen, AnnotationKind::Stroke},
                                       std::pair{SnapTool::Highlighter, AnnotationKind::Stroke});
    const std::optional<Annotation> draft = newDraft(tool, QPointF(4, 5), coral());

    REQUIRE(draft.has_value());
    CHECK(draft->kind == kind);
    CHECK(draft->color == coral());
}

// from tests/test_snap_annotations.py::TestDrafts::test_click_tools_have_no_draft
TEST_CASE("click tools have no draft", "[test_snap_annotations]") {
    const SnapTool tool = GENERATE(SnapTool::Text, SnapTool::Step);
    CHECK_FALSE(newDraft(tool, QPointF(4, 5), coral()).has_value());
}

// from tests/test_snap_annotations.py::TestDrafts::test_short_drags_are_clicks_but_a_pen_dot_counts
TEST_CASE("short drags are clicks but a pen dot counts", "[test_snap_annotations]") {
    Annotation arrow = *newDraft(SnapTool::Arrow, QPointF(0, 0), coral());
    extendAnnotation(arrow, QPointF(3, 2));
    Annotation box = *newDraft(SnapTool::Box, QPointF(0, 0), coral());
    extendAnnotation(box, QPointF(40, 2));
    const Annotation dot = *newDraft(SnapTool::Pen, QPointF(0, 0), coral());

    CHECK_FALSE(meaningful(arrow));
    CHECK_FALSE(meaningful(box));
    CHECK(meaningful(dot));
    extendAnnotation(arrow, QPointF(30, 0));
    extendAnnotation(box, QPointF(40, 30));
    CHECK((meaningful(arrow) && meaningful(box)));
}

// from tests/test_snap_annotations.py::TestDrafts::test_the_highlighter_is_a_wide_stroke
TEST_CASE("the highlighter is a wide stroke", "[test_snap_annotations]") {
    const Annotation pen = *newDraft(SnapTool::Pen, QPointF(0, 0), coral());
    const Annotation marker = *newDraft(SnapTool::Highlighter, QPointF(0, 0), coral());

    CHECK(strokeWidth(pen) == kPenWidth);
    CHECK(strokeWidth(marker) == kHighlighterWidth);
    CHECK((marker.highlighter && !pen.highlighter));
}

// from tests/test_snap_annotations.py::TestDrafts::test_pen_skips_sub_pixel_jitter
TEST_CASE("pen skips sub-pixel jitter", "[test_snap_annotations]") {
    Annotation stroke = *newDraft(SnapTool::Pen, QPointF(0, 0), coral());
    extendAnnotation(stroke, QPointF(0.5, 0.5));
    extendAnnotation(stroke, QPointF(10, 0));

    CHECK(stroke.points == QList<QPointF>{QPointF(0, 0), QPointF(10, 0)});
}

// from tests/test_snap_annotations.py::TestDrafts::test_drafts_copy_the_colour
TEST_CASE("drafts copy the colour", "[test_snap_annotations]") {
    QColor color(coral());
    const Annotation draft = *newDraft(SnapTool::Box, QPointF(0, 0), color);
    color.setRed(0);

    CHECK(draft.color == coral());
}

// from tests/test_snap_annotations.py::TestDrafts::test_an_empty_label_is_not_a_mark
TEST_CASE("an empty label is not a mark", "[test_snap_annotations]") {
    CHECK_FALSE(meaningful(Annotation::label(QPointF(0, 0), QStringLiteral("   "), coral())));
    CHECK(meaningful(Annotation::label(QPointF(0, 0), QStringLiteral("Click here"), coral())));
}

// from tests/test_snap_annotations.py::test_shift_drag_makes_squares_and_circles_in_every_direction
TEST_CASE("shift drag makes squares and circles in every direction", "[test_snap_annotations]") {
    const SnapTool tool = GENERATE(SnapTool::Box, SnapTool::Ellipse);
    const QPointF point =
        GENERATE(QPointF(160, 125), QPointF(40, 125), QPointF(160, 75), QPointF(40, 75));
    Annotation draft = *newDraft(tool, QPointF(100, 100), coral());
    extendDraft(draft, point, true);
    CHECK(annotationRect(draft).width() == 60);
    CHECK(annotationRect(draft).height() == 60);
    // Releasing Shift restores a free-form rectangle or oval on the next move.
    extendDraft(draft, point);
    CHECK(annotationRect(draft).width() == 60);
    CHECK(annotationRect(draft).height() == 25);
}

// from tests/test_snap_annotations.py::test_constrained_shapes_fit_inside_the_capture_bounds
TEST_CASE("constrained shapes fit inside the capture bounds", "[test_snap_annotations]") {
    const SnapTool tool = GENERATE(SnapTool::Box, SnapTool::Ellipse, SnapTool::Arrow);
    const QRectF bounds(0, 0, 120, 120);
    Annotation draft = *newDraft(tool, QPointF(110, 100), coral());
    extendDraft(draft, QPointF(160, 150), true, bounds);
    CHECK(bounds.contains(draft.end));
    CHECK_THAT(draft.end.x() - draft.start.x(),
               Catch::Matchers::WithinRel(draft.end.y() - draft.start.y(), 1e-6));
}

// from tests/test_snap_annotations.py::test_shift_arrows_align_to_45_degree_bearings
TEST_CASE("shift arrows align to 45 degree bearings", "[test_snap_annotations]") {
    const auto [end, axis] =
        GENERATE(std::pair{QPointF(150, 110), 'h'}, std::pair{QPointF(110, 150), 'v'},
                 std::pair{QPointF(150, 140), 'd'});
    Annotation draft = *newDraft(SnapTool::Arrow, QPointF(100, 100), coral());
    extendDraft(draft, end, true);
    const double dx = draft.end.x() - 100;
    const double dy = draft.end.y() - 100;
    if (axis == 'h') {
        CHECK_THAT(dy, Catch::Matchers::WithinAbs(0, 1e-12));
    } else if (axis == 'v') {
        CHECK_THAT(dx, Catch::Matchers::WithinAbs(0, 1e-12));
    } else {
        CHECK_THAT(dx, Catch::Matchers::WithinRel(dy, 1e-6));
    }
}

// from tests/test_snap_annotations.py::TestArrowShape::test_the_head_points_at_the_end
TEST_CASE("the head points at the end", "[test_snap_annotations]") {
    const auto [shaft, head] = arrowShapes(QPointF(0, 100), QPointF(200, 100), 4.0);
    CHECK(head[0] == QPointF(200, 100));
}

// from tests/test_snap_annotations.py::TestArrowShape::test_horizontal_arrows_bow_upward_either_way
TEST_CASE("horizontal arrows bow upward either way", "[test_snap_annotations]") {
    const auto [start, end] = GENERATE(std::pair{QPointF(0, 100), QPointF(200, 100)},
                                       std::pair{QPointF(200, 100), QPointF(0, 100)});
    const auto [shaft, head] = arrowShapes(start, end, 4.0);

    CHECK(shaft.boundingRect().top() < 100 - 5);
    CHECK(shaft.boundingRect().bottom() <= 100 + 0.01);
}

// from tests/test_snap_annotations.py::TestArrowShape::test_vertical_arrows_bow_left
TEST_CASE("vertical arrows bow left", "[test_snap_annotations]") {
    const auto [start, end] = GENERATE(std::pair{QPointF(100, 0), QPointF(100, 200)},
                                       std::pair{QPointF(100, 200), QPointF(100, 0)});
    const auto [shaft, head] = arrowShapes(start, end, 4.0);

    CHECK(shaft.boundingRect().left() < 100 - 5);
}

// from tests/test_snap_annotations.py::TestArrowShape::test_the_shaft_ends_inside_the_head
TEST_CASE("the shaft ends inside the head", "[test_snap_annotations]") {
    const auto [shaft, head] = arrowShapes(QPointF(0, 100), QPointF(200, 100), 4.0);

    CHECK(head.containsPoint(shaft.currentPosition(), Qt::OddEvenFill));
    CHECK(shaft.currentPosition().x() < 200);
}

// from tests/test_snap_annotations.py::TestArrowShape::test_a_zero_length_arrow_has_no_head
TEST_CASE("a zero length arrow has no head", "[test_snap_annotations]") {
    const auto [shaft, head] = arrowShapes(QPointF(5, 5), QPointF(5, 5), 4.0);
    CHECK(head.isEmpty());
}

// from tests/test_snap_annotations.py::test_badge_ink_is_white_on_dark_fills_and_dark_on_amber
TEST_CASE("badge ink is white on dark fills and dark on amber", "[test_snap_annotations]") {
    std::map<std::string, bool> light;
    for (const Swatch& swatch : kSwatches) {
        light[QString::fromUtf16(swatch.name).toStdString()] =
            inkFor(QColor(QString::fromLatin1(swatch.color))).lightness() > 128;
    }
    CHECK(light == std::map<std::string, bool>{
                       {"Coral", true}, {"Amber", false}, {"Blue", true}, {"Green", true}});
}

// from tests/test_snap_annotations.py::test_marks_paint_inside_their_bounds
TEST_CASE("marks paint inside their bounds", "[test_snap_annotations]") {
    const auto [name, item] = GENERATE_COPY(
        std::pair{"arrow", Annotation::arrow(QPointF(40, 120), QPointF(200, 60), coral())},
        std::pair{"arrow-back", Annotation::arrow(QPointF(200, 60), QPointF(40, 140), coral())},
        std::pair{"box", Annotation::box(QPointF(30, 30), QPointF(120, 90), coral())},
        std::pair{"box-filled",
                  Annotation::box(QPointF(30, 30), QPointF(120, 90), coral(), kBoxWidth, true)},
        std::pair{"ellipse",
                  Annotation::ellipse(QPointF(30, 30), QPointF(120, 90), coral(), kBoxWidth, true)},
        std::pair{"redact", Annotation::redaction(QPointF(30, 30), QPointF(120, 90))},
        std::pair{"pen", Annotation::stroke({QPointF(30, 100), QPointF(60, 40), QPointF(90, 120),
                                             QPointF(140, 50)},
                                            coral())},
        std::pair{"highlighter",
                  Annotation::stroke({QPointF(30, 100), QPointF(160, 104)}, coral(), true)},
        std::pair{"step", Annotation::step(QPointF(100, 100), 7, coral())},
        std::pair{"label",
                  Annotation::label(QPointF(20, 20), QStringLiteral("Click here first"), coral())});
    CAPTURE(name);
    // The overlay repaints only the bounds while drawing; ink outside them smears.
    QImage image = transparent(260, 200);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        paintAnnotation(painter, item);
    }

    CHECK(annotationBounds(item).adjusted(-1, -1, 1, 1).contains(QRectF(paintedRect(image))));
}

namespace {

Annotation markOf(const fv::golden::Case& c) {
    const std::string kind = c.arg<std::string>(0);
    if (kind == "arrow") {
        return Annotation::arrow(fvtest::pointFOf(c.args()[1]), fvtest::pointFOf(c.args()[2]),
                                 coral(), c.arg<double>(3));
    }
    if (kind == "box") {
        return Annotation::box(fvtest::pointFOf(c.args()[1]), fvtest::pointFOf(c.args()[2]),
                               coral(), c.arg<double>(3));
    }
    if (kind == "ellipse") {
        return Annotation::ellipse(fvtest::pointFOf(c.args()[1]), fvtest::pointFOf(c.args()[2]),
                                   coral(), c.arg<double>(3));
    }
    if (kind == "redaction") {
        return Annotation::redaction(fvtest::pointFOf(c.args()[1]), fvtest::pointFOf(c.args()[2]));
    }
    if (kind == "stroke") {
        QList<QPointF> points;
        for (const auto& point : c.args()[1]) {
            points.append(fvtest::pointFOf(point));
        }
        return Annotation::stroke(points, coral(), c.arg<bool>(2));
    }
    if (kind == "step") {
        return Annotation::step(fvtest::pointFOf(c.args()[1]), c.arg<int>(2), coral());
    }
    return Annotation::label(QPointF(0, 0), QString::fromStdString(c.arg<std::string>(1)), coral(),
                             QFont());
}

fv::Json elementsOf(const QPainterPath& path) {
    fv::Json elements = fv::Json::array();
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        elements.push_back(fv::Json::array({element.x, element.y, int(element.type)}));
    }
    return elements;
}

SnapTool toolOf(const std::string& name) {
    for (const SnapTool tool :
         {SnapTool::Arrow, SnapTool::Box, SnapTool::Ellipse, SnapTool::Pen, SnapTool::Highlighter,
          SnapTool::Text, SnapTool::Step, SnapTool::Redact}) {
        if (name == toolName(tool)) {
            return tool;
        }
    }
    throw std::invalid_argument("no tool " + name);
}

} // namespace

TEST_CASE("bounds matches OpenWhisper", "[golden][test_snap_annotations]") {
    fv::golden::for_each(kModule, "bounds", [](const fv::golden::Case& c) {
        CHECK(fv::golden::matches(c.result<fv::Json>(), fvtest::listOf(annotationBounds(markOf(c))),
                                  kTolerance));
    });
}

TEST_CASE("meaningful matches OpenWhisper", "[golden][test_snap_annotations]") {
    fv::golden::for_each(kModule, "meaningful", [](const fv::golden::Case& c) {
        CHECK(meaningful(markOf(c)) == c.result<bool>());
    });
}

TEST_CASE("extend_draft matches OpenWhisper", "[golden][test_snap_annotations]") {
    fv::golden::for_each(kModule, "extend_draft", [](const fv::golden::Case& c) {
        Annotation draft =
            *newDraft(toolOf(c.arg<std::string>(0)), fvtest::pointFOf(c.args()[1]), coral());
        for (const auto& move : c.args()[2]) {
            std::optional<QRectF> bounds;
            if (!move[2].is_null()) {
                const auto v = fv::golden::decode<std::vector<double>>(move[2]);
                bounds = QRectF(v.at(0), v.at(1), v.at(2), v.at(3));
            }
            extendDraft(draft, fvtest::pointFOf(move[0]), move[1].get<bool>(), bounds);
        }
        fv::Json actual;
        if (draft.kind == AnnotationKind::Stroke) {
            actual = fv::Json::array();
            for (const QPointF& point : draft.points) {
                actual.push_back(fvtest::listOf(point));
            }
        } else {
            actual = fvtest::listOf(draft.end);
        }
        CHECK(fv::golden::matches(c.result<fv::Json>(), actual, kTolerance));
    });
}

TEST_CASE("arrow_shapes matches OpenWhisper", "[golden][test_snap_annotations]") {
    fv::golden::for_each(kModule, "arrow_shapes", [](const fv::golden::Case& c) {
        const auto [shaft, head] = arrowShapes(fvtest::pointFOf(c.args()[0]),
                                               fvtest::pointFOf(c.args()[1]), c.arg<double>(2));
        fv::Json points = fv::Json::array();
        for (const QPointF& point : head) {
            points.push_back(fvtest::listOf(point));
        }
        const fv::Json actual{{"shaft", elementsOf(shaft)},
                              {"head", points},
                              {"current", fvtest::listOf(shaft.currentPosition())}};
        CHECK(fv::golden::matches(c.result<fv::Json>(), actual, kTolerance));
    });
}

TEST_CASE("smooth_path matches OpenWhisper", "[golden][test_snap_annotations]") {
    fv::golden::for_each(kModule, "smooth_path", [](const fv::golden::Case& c) {
        std::vector<QPointF> points;
        for (const auto& point : c.args()[0]) {
            points.push_back(fvtest::pointFOf(point));
        }
        CHECK(
            fv::golden::matches(c.result<fv::Json>(), elementsOf(smoothPath(points)), kTolerance));
    });
}

TEST_CASE("ink_for matches OpenWhisper", "[golden][test_snap_annotations]") {
    fv::golden::for_each(kModule, "ink_for", [](const fv::golden::Case& c) {
        const QColor ink = inkFor(QColor(QString::fromStdString(c.arg<std::string>(0))));
        CHECK(ink.name().toStdString() == c.result<std::string>());
    });
}

TEST_CASE("next_step_number matches OpenWhisper", "[golden][test_snap_annotations]") {
    fv::golden::for_each(kModule, "next_step_number", [](const fv::golden::Case& c) {
        AnnotationStack stack;
        std::vector<int> numbers;
        for (const auto& operation : c.args()[0]) {
            const std::string name = operation.get<std::string>();
            if (name == "step") {
                stack.add(Annotation::step(QPointF(10, 10), stack.nextStepNumber(), coral()));
            } else if (name == "mark") {
                stack.add(arrowOf(0, 0, 50, 0));
            } else if (name == "undo") {
                stack.undo();
            } else if (name == "redo") {
                stack.redo();
            }
            numbers.push_back(stack.nextStepNumber());
        }
        CHECK(numbers == c.result<std::vector<int>>());
    });
}
