// The golden vectors of ui_qt.overlays.recording_looks' state and drawing (Bars, Ribbon's and
// the wave's finish, Aurora, Pulse, Dots) and the port of the Pulse ripple test.
//
// Python's drawing runs through a recording painter in the case file; here the geometry each
// look hands a painter is turned into the same list of operations (see the case file), with
// Qt's own path rules applied (a lineTo to the last point is dropped, close adds a line back to
// the start, a gradient stop at an existing position replaces it).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "overlay_test_support.h"
#include "uimath/look_sims.h"
#include "uimath/loudness.h"
#include "uimath/overlay_look.h"
#include "uimath/voice_ribbon.h"

using fv::Json;
using fv::PointD;
using fv::Rgba;
using fv::overlay_test::matches_result;
using fv::overlay_test::rgba_json;

namespace {

constexpr char kModule[] = "ui_qt.overlays.recording_looks";

std::vector<double> doubles(const Json& json) {
    return fv::golden::decode<std::vector<double>>(json);
}

// QPainterPath's element list for moveTo(points[0]), lineTo(rest), closeSubpath().
Json path_elements(std::vector<PointD> points, bool close) {
    std::vector<PointD> elements;
    for (const PointD& p : points) {
        if (!elements.empty() && elements.back() == p) {
            continue;
        }
        elements.push_back(p);
    }
    if (close && elements.size() > 1) {
        const PointD first = elements.front();
        PointD& last = elements.back();
        if (!(first == last)) {
            const auto fuzzy = [](double a, double b) {
                return std::fabs(a - b) * 1000000000000.0 <= std::min(std::fabs(a), std::fabs(b));
            };
            if (fuzzy(first.x, last.x) && fuzzy(first.y, last.y)) {
                last = first;
            } else {
                elements.push_back(first);
            }
        }
    }
    return fv::overlay_test::points_json(elements);
}

// QGradient::setColorAt over stops in order: a stop at an existing position replaces it.
Json stops_json(const std::vector<fv::GradientStop>& stops) {
    std::vector<fv::GradientStop> kept;
    for (const fv::GradientStop& stop : stops) {
        std::size_t index = 0;
        while (index < kept.size() && kept[index].position < stop.position) {
            ++index;
        }
        if (index < kept.size() && kept[index].position == stop.position) {
            kept[index].color = stop.color;
        } else {
            kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(index), stop);
        }
    }
    Json out = Json::array();
    for (const fv::GradientStop& stop : kept) {
        out.push_back(Json::array({stop.position, rgba_json(stop.color)}));
    }
    return out;
}

Json linear_json(double left, double right, const std::vector<fv::GradientStop>& stops) {
    return Json{{"kind", "linear"},
                {"start", Json::array({left, 0.0})},
                {"stop", Json::array({right, 0.0})},
                {"stops", stops_json(stops)}};
}

Json pen_json(double width, const Rgba& color) {
    return Json::array({width, rgba_json(color)});
}

Json line_op(const fv::LineSegment& s) {
    return Json::array({"line", s.from.x, s.from.y, s.to.x, s.to.y, pen_json(s.width, s.color)});
}

Json ellipse_op(PointD center, double radius, Json pen, Json brush) {
    return Json::array({"ellipse", center.x, center.y, radius, radius, pen, brush});
}

Json ops_of(const std::vector<fv::Bar>& bars) {
    Json ops = Json::array();
    for (const fv::Bar& bar : bars) {
        ops.push_back(
            Json::array({"rrect", bar.rect.x, bar.rect.y, bar.rect.width, bar.rect.height,
                         fv::kBarWidth / 2, fv::kBarWidth / 2, nullptr, rgba_json(bar.color)}));
    }
    return ops;
}

Json ops_of(const std::vector<fv::LineSegment>& lines) {
    Json ops = Json::array();
    for (const fv::LineSegment& line : lines) {
        ops.push_back(line_op(line));
    }
    return ops;
}

Json ops_of(const fv::RibbonFinish& finish) {
    Json ops = ops_of(finish.segments);
    ops.push_back(
        ellipse_op(finish.head, finish.head_radius, nullptr, rgba_json(finish.head_color)));
    return ops;
}

Json ops_of_frame(const fv::WaveShape& shape) {
    const Json elements = path_elements(shape.outline, true);
    return Json::array(
        {Json::array({"fillpath", elements,
                      linear_json(shape.gradient_left, shape.gradient_right, shape.fill)}),
         Json::array(
             {"path", elements,
              Json::array({shape.edge_pen_width,
                           linear_json(shape.gradient_left, shape.gradient_right, shape.edge)}),
              nullptr})});
}

Json ops_of_finish(const fv::WaveShape& shape) {
    return Json::array(
        {Json::array({"rrect", shape.pill.x, shape.pill.y, shape.pill.width, shape.pill.height,
                      shape.pill_radius, shape.pill_radius, nullptr, rgba_json(shape.pill_color)}),
         Json::array({"path", path_elements(shape.outline, true), nullptr,
                      linear_json(shape.gradient_left, shape.gradient_right, shape.fill)})});
}

Json ops_of(const std::vector<fv::AuroraWave>& waves) {
    Json ops = Json::array();
    for (const fv::AuroraWave& wave : waves) {
        const Json elements = path_elements(wave.points, false);
        ops.push_back(
            Json::array({"path", elements, pen_json(wave.glow_width, wave.glow_color), nullptr}));
        ops.push_back(
            Json::array({"path", elements, pen_json(wave.core_width, wave.core_color), nullptr}));
    }
    return ops;
}

Json circle_op(const fv::PulseCircle& c) {
    return ellipse_op(c.center, c.radius,
                      c.pen_width == 0.0 ? Json(nullptr) : pen_json(c.pen_width, c.pen),
                      c.filled ? rgba_json(c.fill) : Json(nullptr));
}

Json ops_of(const fv::PulseFrame& frame) {
    Json ops = Json::array();
    for (const fv::PulseCircle& ripple : frame.ripples) {
        ops.push_back(circle_op(ripple));
    }
    ops.push_back(circle_op(frame.ring));
    ops.push_back(circle_op(frame.core));
    return ops;
}

Json ops_of(const fv::PulseFinish& finish) {
    Json ops = Json::array();
    for (const fv::PulseCircle& ripple : finish.ripples) {
        ops.push_back(circle_op(ripple));
    }
    ops.push_back(circle_op(finish.ring));
    const Json sweep{{"kind", "conical"},
                     {"center", Json::array({finish.ring.center.x, finish.ring.center.y})},
                     {"angle", finish.sweep_angle_degrees},
                     {"stops", stops_json(finish.sweep)}};
    ops.push_back(ellipse_op(finish.ring.center, finish.ring.radius,
                             Json::array({finish.sweep_pen_width, sweep}), nullptr));
    ops.push_back(circle_op(finish.core));
    return ops;
}

Json ops_of(const std::vector<fv::Dot>& dots) {
    Json ops = Json::array();
    for (const fv::Dot& dot : dots) {
        ops.push_back(ellipse_op(dot.center, dot.halo_radius, nullptr, rgba_json(dot.halo_color)));
        ops.push_back(ellipse_op(dot.center, dot.radius, nullptr, rgba_json(dot.color)));
    }
    return ops;
}

// The ribbon the case file's MirrorWave reads: it heard each level in turn, a frame apart.
fv::VoiceRibbon heard(const std::vector<double>& levels) {
    fv::VoiceRibbon ribbon;
    for (const double level : levels) {
        ribbon.hear(level);
        ribbon.advance(1.0 / 30.0);
    }
    return ribbon;
}

fv::VoiceSnapshot snapshot_of(const Json& values) {
    fv::VoiceSnapshot snapshot;
    snapshot.take(doubles(values));
    return snapshot;
}

} // namespace

TEST_CASE("Bars, the wave and the dots draw the frames Python draws", "[golden][look_sims]") {
    fv::golden::for_each(kModule, "Visual.draw", [](const fv::golden::Case& c) {
        const std::string name = c.arg<std::string>(0);
        const bool dark = c.arg<std::string>(1) == "dark";
        const double hue = c.arg<double>(2);
        const double width = c.arg<double>(3);
        const std::vector<double> history = doubles(c.args().at(5));
        const double grow = c.arg<double>(7);
        Json ops;
        if (name == "Bars") {
            ops = ops_of(fv::bars_frame(history, width, hue, grow, dark));
        } else if (name == "MirrorWave") {
            ops = ops_of_frame(fv::mirror_wave_frame(heard(history), width, hue, grow, dark));
        } else {
            ops = ops_of(fv::dots_frame(history, width, hue, grow, dark));
        }
        CHECK(matches_result(c, ops));
    });
}

TEST_CASE("the finishes of Bars, the ribbon, the wave and the dots draw as Python's",
          "[golden][look_sims]") {
    fv::golden::for_each(kModule, "Visual.draw_work", [](const fv::golden::Case& c) {
        const std::string name = c.arg<std::string>(0);
        const bool dark = c.arg<std::string>(1) == "dark";
        const double hue = c.arg<double>(2);
        const double width = c.arg<double>(3);
        const fv::VoiceSnapshot snapshot = snapshot_of(c.args().at(6));
        const double morph = c.arg<double>(7);
        const double clock = c.arg<double>(8);
        Json ops;
        if (name == "Bars") {
            ops = ops_of(fv::bars_finish(snapshot, width, hue, morph, clock, dark));
        } else if (name == "MirrorWave") {
            ops = ops_of_finish(fv::mirror_wave_finish(snapshot, width, hue, morph, clock, dark));
        } else if (name == "Dots") {
            ops = ops_of(fv::dots_finish(snapshot, width, hue, morph, clock, dark));
        } else {
            // 80 is the overlay's base height.
            ops = ops_of(fv::ribbon_finish(snapshot, width, 80.0 - fv::kBaselineFromBottom, hue,
                                           morph, clock, dark));
        }
        CHECK(matches_result(c, ops));
    });
}

TEST_CASE("Aurora's smoothing, pace and waves replay the golden frames", "[golden][look_sims]") {
    fv::golden::for_each(kModule, "Aurora", [](const fv::golden::Case& c) {
        const bool dark = c.arg<std::string>(2) == "dark";
        const double hue = c.arg<double>(3);
        const double width = c.arg<double>(4);
        fv::AuroraSim aurora;
        Json frames = Json::array();
        for (const Json& frame : c.args().at(0)) {
            aurora.advance(fv::golden::decode<double>(frame.at(1)),
                           fv::golden::decode<double>(frame.at(0)));
            frames.push_back(Json::array({aurora.amplitude(), aurora.phase()}));
        }
        const Json draw = ops_of(aurora.frame(width, hue, c.arg<double>(5), dark));
        aurora.begin_work();
        for (const Json& frame : c.args().at(1)) {
            aurora.advance_work(fv::golden::decode<double>(frame.at(0)),
                                fv::golden::decode<double>(frame.at(1)));
            frames.push_back(Json::array({aurora.amplitude(), aurora.phase()}));
        }
        const Json work =
            ops_of(aurora.finish(width, hue, c.arg<double>(6), c.arg<double>(7), dark));
        CHECK(matches_result(c, Json{{"frames", frames}, {"draw", draw}, {"work", work}}));
    });
}

TEST_CASE("Pulse's ripples and rings replay the golden frames", "[golden][look_sims]") {
    fv::golden::for_each(kModule, "Pulse", [](const fv::golden::Case& c) {
        const bool dark = c.arg<std::string>(2) == "dark";
        const double hue = c.arg<double>(3);
        const double width = c.arg<double>(4);
        fv::PulseSim pulse;
        double level = 0.0;
        Json frames = Json::array();
        const auto record = [&] {
            frames.push_back(Json::array({Json(pulse.ripples()), pulse.time()}));
        };
        for (const Json& frame : c.args().at(0)) {
            level = fv::golden::decode<double>(frame.at(0));
            pulse.advance(fv::golden::decode<double>(frame.at(1)), level);
            record();
        }
        const Json draw = ops_of(pulse.frame(width, level, hue, c.arg<double>(5), dark));
        pulse.begin_work(doubles(c.args().at(8)));
        for (const Json& frame : c.args().at(1)) {
            pulse.advance_work(fv::golden::decode<double>(frame.at(0)),
                               fv::golden::decode<double>(frame.at(1)));
            record();
        }
        const Json work =
            ops_of(pulse.finish(width, hue, c.arg<double>(6), c.arg<double>(7), dark));
        CHECK(matches_result(c, Json{{"frames", frames}, {"draw", draw}, {"work", work}}));
    });
}

TEST_CASE("snap_at reads the frozen voice as Python does", "[golden][look_sims]") {
    fv::golden::for_each(kModule, "Visual.snap_at", [](const fv::golden::Case& c) {
        const fv::VoiceSnapshot snapshot = snapshot_of(c.args().at(0));
        CHECK(matches_result(c, Json(snapshot.at(c.arg<double>(1)))));
    });
}

TEST_CASE("the dots read the voice a few samples apart", "[golden][look_sims]") {
    // Dots._level is the lag: the left dot reads the oldest sample, the right the newest.
    fv::golden::for_each(kModule, "Dots._level", [](const fv::golden::Case& c) {
        const std::vector<double> values = doubles(c.args().at(0));
        const int k = c.arg<int>(1);
        // dots_frame at grow 1 with this history puts dot k at kCenterY - 9 * level.
        std::vector<double> history = values;
        const auto dots = fv::dots_frame(history, 300.0, 0.0, 1.0, true);
        const double level = (fv::kCenterY - dots[static_cast<std::size_t>(k)].center.y) / 9.0;
        CHECK(fv::golden::near(c.result<double>(), level, 1e-12));
    });
}

TEST_CASE("the particles' launch lines follow the wave and the ribbon", "[golden][look_sims]") {
    fv::golden::for_each(kModule, "emitters", [](const fv::golden::Case& c) {
        const fv::VoiceRibbon ribbon = heard(doubles(c.args().at(0)));
        Json rows = Json::array();
        for (const double x : doubles(c.args().at(1))) {
            rows.push_back(Json::array({fv::wave_emitter(ribbon, x, 300.0),
                                        ribbon.crest(x, 300.0, 80.0 - fv::kBaselineFromBottom)}));
        }
        CHECK(matches_result(c, rows));
    });
}

// from tests/test_recording_look.py::test_pulse_ripples_when_a_word_starts
TEST_CASE("Pulse ripples when a word starts", "[test_recording_look][look_sims]") {
    fv::VoiceRibbon ribbon;
    fv::PulseSim pulse;
    double time = 0.0;
    const auto run = [&](double seconds, double level) {
        const double end = time + seconds;
        while (time < end) {
            ribbon.hear(level);
            ribbon.advance(1.0 / 30.0);
            pulse.advance(1.0 / 30.0, ribbon.shown());
            time += 1.0 / 30.0;
        }
    };
    run(0.4, 0.002);
    CHECK(pulse.ripples().empty());
    run(0.2, 0.06);
    CHECK_FALSE(pulse.ripples().empty());
}

TEST_CASE("a ripple needs a rise, a loud word and a pause after the last", "[look_sims]") {
    fv::PulseSim pulse;
    // Loud but not rising: no ripple.
    pulse.advance(0.05, 0.6);
    CHECK(pulse.ripples().size() == 1); // from 0.0 to 0.6 is a rise of more than 0.12
    pulse.advance(0.05, 0.62);
    CHECK(pulse.ripples().size() == 1);
    // A rise inside 0.16 s of the last ripple does not make another.
    pulse.advance(0.03, 0.1);
    pulse.advance(0.03, 0.9);
    CHECK(pulse.ripples().size() == 1);
    // Past 0.16 s it does.
    pulse.advance(0.05, 0.1);
    pulse.advance(0.1, 0.9);
    CHECK(pulse.ripples().size() == 2);
    // A ripple lives 0.8 s.
    pulse.advance(0.85, 0.1);
    CHECK(pulse.ripples().empty());
}

TEST_CASE("Aurora's level eases with a 0.05 s time constant and its pace rises with the voice",
          "[look_sims]") {
    fv::AuroraSim aurora;
    aurora.advance(0.05, 1.0);
    CHECK(aurora.amplitude() == 1.0 - std::exp(-1.0));
    CHECK(aurora.phase() == 0.05 * (2.0 + 7.0));
    fv::AuroraSim quiet;
    quiet.advance(0.05, 0.0);
    CHECK(quiet.phase() == 0.05 * 2.0);
    aurora.reset();
    CHECK(aurora.amplitude() == 0.0);
    CHECK(aurora.phase() == 0.0);
}

TEST_CASE("the snapshot starts as two zeros and replaces itself on each take", "[look_sims]") {
    fv::VoiceSnapshot snapshot;
    CHECK(snapshot.values() == std::vector<double>{0.0, 0.0});
    CHECK(snapshot.at(0.5) == 0.0);
    snapshot.take({0.0, 1.0});
    CHECK(snapshot.at(0.5) == 0.5);
    CHECK(snapshot.at(-3.0) == 0.0);
    CHECK(snapshot.at(7.0) == 1.0);
}

TEST_CASE("an empty voice draws silence instead of reading past the end", "[look_sims]") {
    fv::VoiceSnapshot snapshot;
    snapshot.take({});
    CHECK(snapshot.at(0.0) == 0.0);
    CHECK(snapshot.at(1.0) == 0.0);
    const std::vector<double> none;
    for (const fv::Bar& bar : fv::bars_frame(none, 300.0, 0.0, 1.0, true)) {
        CHECK(bar.rect.height == 3.0); // (1.5 + 18 * 0) * 2
    }
    for (const fv::Dot& dot : fv::dots_frame(none, 300.0, 0.0, 1.0, true)) {
        CHECK(dot.center.y == fv::kCenterY);
    }
    CHECK(fv::dots_finish(snapshot, 300.0, 0.0, 0.0, 0.0, true).size() ==
          static_cast<std::size_t>(fv::kDotCount));
}
