// Outcome notices near the pointer (tests/test_personalize_fix3_h1_outcome_notices.py, the
// overlay's own tests; the controller's wording is tested where the controller is).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QFont>
#include <QFontMetrics>
#include <QRect>

#include "overlay_test_support.h"
#include "uimath/moments.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_notice_shows_while_hidden_and_then_hides
TEST_CASE("a notice shows while hidden and then hides",
          "[widget][test_personalize_fix3_h1_outcome_notices]") {
    Overlay overlay;
    overlay->showNotice(QStringLiteral("Select the text to change first"));
    CHECK(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateNotice);
    CHECK(overlay.access.notice() == QStringLiteral("Select the text to change first"));
    CHECK(overlay.access.hiddenTimer().isActive());
    CHECK(overlay.access.hiddenTimer().interval() >= WaveformOverlay::kCaptionMs);
    fvtest::paint(overlay);
    fvtest::fireTimeout(overlay.access.hiddenTimer());
    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay.access.notice().isEmpty());
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_long_notices_stay_up_longer_and_still_paint
TEST_CASE("long notices stay up longer and still paint",
          "[widget][test_personalize_fix3_h1_outcome_notices]") {
    Overlay overlay;
    overlay->showNotice(
        QStringLiteral("Rewrite copied — couldn't confirm the app; paste it where you want"));
    const int longWait = overlay.access.hiddenTimer().interval();
    fvtest::paint(overlay);
    overlay->showNotice(QStringLiteral("Rewritten"), true);
    CHECK(overlay.access.hiddenTimer().interval() < longWait);
    fvtest::paint(overlay);
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_finished_command_bursts_after_its_sparkles_gather_and_replays
TEST_CASE("a finished command bursts after its sparkles gather and replays",
          "[widget][test_personalize_fix3_h1_outcome_notices]") {
    Overlay overlay;
    const fv::Moment& done = *fv::find_moment("done");
    overlay->showNotice(QStringLiteral("Rewritten"), true);
    overlay->timer().stop();
    overlay.access.animationTime() = done.pop_at / 2;
    overlay.access.updateAnimation();
    CHECK(overlay.access.sttParticles().empty());
    overlay.access.animationTime() = done.pop_at;
    overlay.access.updateAnimation();
    CHECK_FALSE(overlay.access.sttParticles().empty());
    for (const double moment : {0.1, 0.3, 0.6, 1.5, overlay.access.noticeMs() / 1000.0}) {
        overlay.access.animationTime() = moment;
        fvtest::paint(overlay);
    }

    overlay->showNotice(QStringLiteral("Inserted"), true);
    CHECK(overlay.access.animationTime() == 0.0);
    CHECK(overlay.access.sttParticles().empty());
    overlay->showNotice(QStringLiteral("Nothing to paste yet"));
    overlay.access.animationTime() = 1.0;
    overlay.access.updateAnimation();
    CHECK(overlay.access.sttParticles().empty());
}

// show_notice's first branches (waveform_overlay.py:1552-1557): a recording or a job already on
// screen keeps its overlay and takes the text as a caption; an empty text is ignored.
TEST_CASE("a notice over a recording or a job becomes its caption", "[widget]") {
    const QString state =
        GENERATE(WaveformOverlay::kStateRecording, WaveformOverlay::kStateCommandListening,
                 WaveformOverlay::kStateProcessing, WaveformOverlay::kStateTranscribing,
                 WaveformOverlay::kStateCleaning, WaveformOverlay::kStateRewriting);
    CAPTURE(state.toStdString());
    Overlay overlay;
    overlay->showAtCursor(state);
    overlay->showNotice(QStringLiteral("Nothing to paste yet"));
    CHECK(overlay->currentState() == state);
    CHECK(overlay.access.caption() == QStringLiteral("Nothing to paste yet"));
    CHECK(overlay.access.notice().isEmpty());
    fvtest::paint(overlay);
}

TEST_CASE("an empty notice is ignored", "[widget]") {
    Overlay overlay;
    overlay->showNotice(QString());
    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateIdle);

    overlay->showNotice(QStringLiteral("Copied"), true);
    overlay->showNotice(QString());
    CHECK(overlay.access.notice() == QStringLiteral("Copied"));
}

// A notice's time on screen: CAPTION_MS and 25 ms for each character, counted as Python's len()
// counts them (code points, not UTF-16 units).
TEST_CASE("a notice stays up 25 ms longer for each character", "[widget]") {
    Overlay overlay;
    overlay->showNotice(QStringLiteral("Saved"));
    CHECK(overlay.access.hiddenTimer().interval() == WaveformOverlay::kCaptionMs + 5 * 25);
    overlay.access.finishHide();
    // Two characters outside the BMP are two code points, four UTF-16 units.
    overlay->showNotice(QString::fromUtf8("\xF0\x9F\x8E\x89\xF0\x9F\x8E\x89"));
    CHECK(overlay.access.hiddenTimer().interval() == WaveformOverlay::kCaptionMs + 2 * 25);
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_notice_too_long_for_the_overlay_ends_in_an_ellipsis
TEST_CASE("a notice too long for the overlay ends in an ellipsis",
          "[widget][test_personalize_fix3_h1_outcome_notices]") {
    Overlay overlay;
    QString text = QStringLiteral("Error: The AI model failed: ");
    for (int i = 0; i < 10; ++i) {
        text += QStringLiteral("rate limit reached ");
    }
    overlay->showNotice(text);
    const QFont font(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
    const QRect rect(0, 0, 240, 64);
    const QString shown = overlay.access.fitNotice(font, rect);
    CHECK(shown.endsWith(QStringLiteral("…")));
    CHECK(shown.startsWith(QStringLiteral("Error: The AI model failed:")));
    const int flags = Qt::AlignLeft | Qt::TextWordWrap;
    CHECK(QFontMetrics(font).boundingRect(rect, flags, shown).height() <= rect.height());
}
