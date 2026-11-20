// The overlay's one-shot moments share one choreography (tests/test_overlay_moments.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <numeric>
#include <string>

#include <QPointF>

#include "overlay_test_support.h"
#include "ui_overlay/moments_painter.h"
#include "uimath/moments.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

const fv::ui::Palette& themeFor(const std::string& name) {
    return name == "dark" ? fv::ui::darkPalette() : fv::ui::lightPalette();
}

} // namespace

// from tests/test_overlay_moments.py::test_each_toggle_and_copy_pops_its_moment
TEST_CASE("each toggle and copy pops its moment", "[widget][test_overlay_moments]") {
    const std::string theme = GENERATE("dark", "light");
    const auto [state, name] = GENERATE(table<QString, std::string>({
        {WaveformOverlay::kStateSttEnable, "enabled"},
        {WaveformOverlay::kStateSttDisable, "disabled"},
        {WaveformOverlay::kStateCopied, "copied"},
    }));
    CAPTURE(theme, name);
    fvtest::ScopedPalette palette(themeFor(theme));
    Overlay overlay;
    overlay->showAtCursor(state);
    const fv::Moment* expected = fv::find_moment(name);
    REQUIRE(expected != nullptr);
    CHECK(overlay.access.moment() == expected);
    if (expected->pop_at != 0.0) {
        overlay->timer().stop();
        overlay.access.animationTime() = expected->pop_at / 2;
        overlay.access.updateAnimation();
        // The confetti waits for the disc.
        CHECK(overlay.access.sttParticles().empty());
    }
    fvtest::paintThrough(overlay, expected->pop_at + 0.05);
    CHECK_FALSE(overlay.access.sttParticles().empty());
    fvtest::paintThrough(overlay, overlay.access.transientMs() / 1000.0);
}

// from tests/test_overlay_moments.py::test_a_cancel_implodes_the_recording_then_slams_and_bursts
TEST_CASE("a cancel implodes the recording then slams and bursts",
          "[widget][test_overlay_moments]") {
    const std::string theme = GENERATE("dark", "light");
    CAPTURE(theme);
    fvtest::ScopedPalette palette(themeFor(theme));
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->updateAudioLevels(QList<float>(20, 0.5f));
    fvtest::paintThrough(overlay, 0.5);
    overlay->setState(WaveformOverlay::kStateCanceling);
    // What was on screen is kept to implode.
    CHECK_FALSE(overlay.access.collapseFrom().empty());
    fvtest::paintThrough(overlay, fv::canceled_moment().pop_at / 2);
    CHECK(overlay.access.sttParticles().empty());
    fvtest::paintThrough(overlay, fv::canceled_moment().pop_at + 0.05);
    CHECK(overlay.access.sttParticles().size() > 40);
    fvtest::paintThrough(overlay, fv::WaveformOverlayTestAccess::cancelSeconds() + 0.1);
    CHECK(overlay->currentState() == WaveformOverlay::kStateIdle);
}

// from tests/test_overlay_moments.py::test_a_cancel_from_hidden_still_plays
TEST_CASE("a cancel from hidden still plays", "[widget][test_overlay_moments]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateCanceling);
    CHECK(overlay.access.collapseFrom().empty());
    fvtest::paintThrough(overlay, fv::WaveformOverlayTestAccess::cancelSeconds() + 0.1);
    CHECK(overlay->currentState() == WaveformOverlay::kStateIdle);
}

// from tests/test_overlay_moments.py::test_switching_off_lets_its_confetti_fall
TEST_CASE("switching off lets its confetti fall", "[widget][test_overlay_moments]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateSttDisable);
    fvtest::paintThrough(overlay, 0.1);
    const auto meanVy = [](const std::vector<fv::Particle>& particles) {
        double sum = 0.0;
        for (const fv::Particle& particle : particles) {
            sum += particle.vy;
        }
        return sum / static_cast<double>(particles.size());
    };
    const std::vector<fv::Particle> early = overlay.access.sttParticles();
    REQUIRE_FALSE(early.empty());
    fvtest::paintThrough(overlay, 0.4);
    REQUIRE_FALSE(overlay.access.sttParticles().empty());
    CHECK(meanVy(overlay.access.sttParticles()) > meanVy(early));
}

// from tests/test_overlay_moments.py::test_a_moment_fades_out_before_it_hides
TEST_CASE("a moment fades out before it hides", "[test_overlay_moments]") {
    CHECK(fv::exit_opacity(0.2, 1.5) == 1.0);
    CHECK(0.0 < fv::exit_opacity(1.4, 1.5));
    CHECK(fv::exit_opacity(1.4, 1.5) < 1.0);
    CHECK(fv::exit_opacity(1.5, 1.5) == 0.0);
}

// from tests/test_overlay_moments.py::test_a_problem_notice_shakes_but_never_celebrates
TEST_CASE("a problem notice shakes but never celebrates", "[widget][test_overlay_moments]") {
    const std::string theme = GENERATE("dark", "light");
    CAPTURE(theme);
    fvtest::ScopedPalette palette(themeFor(theme));
    Overlay overlay;
    overlay->showNotice(QStringLiteral("Select the text to change first"));
    fvtest::paintThrough(overlay, overlay.access.transientMs() / 1000.0);
    CHECK(overlay.access.moment() == nullptr);
    CHECK(overlay.access.sttParticles().empty());
}

// from
// tests/test_overlay_moments.py::test_reconnect_status_and_failure_remain_visible_on_the_overlay
TEST_CASE("reconnect status and failure remain visible on the overlay",
          "[widget][test_overlay_moments]") {
    const std::string theme = GENERATE("dark", "light");
    CAPTURE(theme);
    fvtest::ScopedPalette palette(themeFor(theme));
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateProcessing);
    QString label;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        label = QStringLiteral("Retrying connection (%1/3)...").arg(attempt);
        overlay->setProcessingLabel(label);
        CHECK(overlay.access.workLabel() == label);
        fvtest::paint(overlay);
    }
    overlay->setState(WaveformOverlay::kStateTranscribing);
    CHECK(overlay.access.labelFrom() == label);
    CHECK(overlay.access.workLabel() == QStringLiteral("Transcribing..."));

    overlay->hide();
    CHECK(overlay.access.fadingSince().has_value());
    overlay->showNotice(QStringLiteral("Reconnect failed. Audio saved in History."));
    CHECK(overlay->currentState() == WaveformOverlay::kStateNotice);
    CHECK_FALSE(overlay.access.fadingSince().has_value());
    fvtest::paintThrough(overlay, 0.5);
    CHECK(overlay->isVisible());
}

// from tests/test_overlay_moments.py::test_glyphs_draw_from_nothing_to_whole
TEST_CASE("glyphs draw from nothing to whole", "[test_overlay_moments]") {
    const QPointF center(50, 40);
    for (const char* glyph : {"check", "cross", "bang", "clip"}) {
        CAPTURE(glyph);
        CHECK(fv::glyphPainterPath(glyph, center, 0.0).length() == 0.0);
        const double half = fv::glyphPainterPath(glyph, center, 0.5).length();
        const double whole = fv::glyphPainterPath(glyph, center, 1.0).length();
        CHECK(0.0 < half);
        CHECK(half < whole);
    }
}

// from tests/test_overlay_moments.py::test_the_language_notice_and_the_split_still_paint
TEST_CASE("the language notice and the split still paint", "[widget][test_overlay_moments]") {
    const std::string theme = GENERATE("dark", "light");
    CAPTURE(theme);
    fvtest::ScopedPalette palette(themeFor(theme));
    Overlay overlay;
    overlay->setLanguage(QStringLiteral("es"), {QStringLiteral("en"), QStringLiteral("es")});
    overlay->showLanguageNotice();
    CHECK(overlay->currentState() == WaveformOverlay::kStateLanguage);
    fvtest::paintThrough(overlay, 1.5);
    overlay->hide();
    overlay->setLargeFileInfo(48.2);
    overlay->showAtCursor(WaveformOverlay::kStateLargeFileSplitting);
    CHECK(overlay->currentState() == WaveformOverlay::kStateLargeFileSplitting);
    fvtest::paintThrough(overlay, 3.0);
}
