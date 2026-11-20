// The recording overlay's live look, its legacy option and their finishes
// (tests/test_recording_look.py). The looks' own tests are test_looks.cpp; the Settings
// preview's are test_look_preview.cpp.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <string>
#include <vector>

#include <QImage>
#include <QRect>

#include "overlay_test_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "uimath/live_label.h"
#include "uimath/loudness.h"
#include "uimath/overlay_look.h"
#include "uimath/tidy_lines.h"

using Catch::Approx;
using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

// A word on a desk microphone, as raw RMS, and a quiet room.
constexpr float kSpeech = 0.06f;
constexpr float kSilence = 0.002f;

// recording_looks.LOOKS, in order.
QString anyLook() {
    std::vector<QString> looks;
    for (const std::string_view look : fv::kLooks) {
        looks.push_back(QString::fromUtf8(look.data(), static_cast<qsizetype>(look.size())));
    }
    return GENERATE_COPY(from_range(looks));
}

const fv::ui::Palette& themeFor(const std::string& name) {
    return name == "dark" ? fv::ui::darkPalette() : fv::ui::lightPalette();
}

std::string lookSetting(const fv::Json& saved) {
    return fv::keys::recording_overlay_look.read(saved);
}

// _finish: a recording in look, then Processing.
void finish(Overlay& overlay, const QString& look) {
    overlay->setRecordingLook(look);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::run(overlay, 0.8, kSpeech);
    overlay->setState(WaveformOverlay::kStateProcessing);
}

} // namespace

// from tests/test_recording_look.py::test_bars_is_the_default_and_unknown_values_fall_back_to_it
TEST_CASE("bars is the default and unknown values fall back to it", "[test_recording_look]") {
    using fv::Json;
    CHECK(lookSetting(Json::object()) == "bars");
    CHECK(lookSetting(Json{{"recording_overlay_look", "sparkly"}}) == "bars");
    CHECK(lookSetting(Json{{"recording_overlay_look", "legacy"}}) == "legacy");
    // Saved before the looks split.
    CHECK(lookSetting(Json{{"recording_overlay_look", "live"}}) == "ribbon");
    CHECK(fv::normalize_look("") == fv::kLookBars);
    CHECK(fv::normalize_look("live") == fv::kLookRibbon);
    // Orb and Bubble lift were replaced; a saved choice lands on the successor.
    CHECK(lookSetting(Json{{"recording_overlay_look", "orb"}}) == "pulse");
    CHECK(lookSetting(Json{{"recording_overlay_look", "bubbles"}}) == "baitball");
    CHECK(fv::normalize_look("orb") == fv::kLookPulse);
}

// from tests/test_recording_look.py::test_the_label_parts_default_on
TEST_CASE("the label parts default on", "[test_recording_look]") {
    const fv::Json empty = fv::Json::object();
    CHECK(fv::keys::recording_overlay_dot.read(empty));
    CHECK(fv::keys::recording_overlay_text.read(empty));
    CHECK(fv::keys::recording_overlay_clock.read(empty));
}

// from tests/test_recording_look.py::test_the_live_look_hears_speech_the_legacy_one_barely_does
TEST_CASE("the live look hears speech the legacy one barely does",
          "[widget][test_recording_look]") {
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("ribbon"));
    overlay->updateAudioLevels(QList<float>(20, kSpeech));
    CHECK(overlay.access.style().audio_levels()[0] == Approx(fv::level_to_height(kSpeech)));
    overlay->setRecordingLook(QStringLiteral("legacy"));
    overlay->updateAudioLevels(QList<float>(20, kSpeech));
    CHECK(overlay.access.style().audio_levels()[0] == static_cast<double>(kSpeech));
    CHECK_FALSE(overlay.access.style().emitter);
    CHECK(fv::level_to_height(kSpeech) > 10 * static_cast<double>(kSpeech));
}

// from tests/test_recording_look.py::test_every_listening_state_paints_in_both_looks
TEST_CASE("every listening state paints in both looks", "[widget][test_recording_look]") {
    const std::string theme = GENERATE("dark", "light");
    const QString look = anyLook();
    const QString state =
        GENERATE(WaveformOverlay::kStateRecording, WaveformOverlay::kStateStreaming,
                 WaveformOverlay::kStateCommandListening);
    const fv::PreviewLayout layout = GENERATE(fv::PreviewLayout::Header, fv::PreviewLayout::Footer);
    CAPTURE(theme, look.toStdString(), state.toStdString(), layout == fv::PreviewLayout::Footer);
    fvtest::ScopedPalette palette(themeFor(theme));
    Overlay overlay;
    overlay.access.streamingLayout() = layout;
    overlay->setRecordingLook(look);
    overlay->showAtCursor(state);
    fvtest::run(overlay, 0.6, kSpeech);
    if (state != WaveformOverlay::kStateRecording) {
        overlay->updateStreamingText(QStringLiteral("hello there, this is a live preview"));
        fvtest::run(overlay, 0.2, kSpeech);
    }
    overlay->showCaption(QStringLiteral("Switched to the USB microphone"));
    fvtest::run(overlay, 0.2, kSilence);
}

// from tests/test_recording_look.py::test_a_new_recording_restarts_the_clock_and_clears_the_ribbon
TEST_CASE("a new recording restarts the clock and clears the ribbon",
          "[widget][test_recording_look]") {
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("ribbon"));
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::run(overlay, 0.5, kSpeech);
    overlay.access.listenStarted() -= 30;
    overlay->setState(WaveformOverlay::kStateTranscribing);
    overlay->setState(WaveformOverlay::kStateRecording);
    CHECK(overlay.access.now() - overlay.access.listenStarted() < 1.0);
    CHECK(overlay.access.ribbon().height_at(285, 300) == 0.0);
}

// from tests/test_recording_look.py::test_the_live_label_reads_a_running_clock
TEST_CASE("the live label reads a running clock", "[test_recording_look]") {
    CHECK(fv::format_clock(0.4) == "0:00");
    CHECK(fv::format_clock(65.2) == "1:05");
}

// _draw_live_listening's grow (waveform_overlay.py:389): the bars grow in from 0.05 s to 0.45 s
// into the recording, on the clock rather than the frame count.
TEST_CASE("the bars grow in as the recording starts", "[widget]") {
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("bars"));
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->timer().stop();
    for (int i = 0; i < 70; ++i) {
        // A voice, without time passing.
        overlay.access.ribbon().hear(kSpeech);
        overlay.access.ribbon().advance(1.0 / 30);
    }
    // Between the edge flash and the label row.
    const QRect band(20, 8, overlay->width() - 40, 40);
    const QImage before = overlay->grab().toImage().copy(band);
    fvtest::run(overlay, 0.6, kSpeech);
    CHECK(overlay.access.now() - overlay.access.listenStarted() >= 0.45);
    const QImage after = overlay->grab().toImage().copy(band);
    REQUIRE(before.size() == after.size());
    int differing = 0;
    for (int y = 0; y < before.height(); ++y) {
        for (int x = 0; x < before.width(); ++x) {
            differing += before.pixel(x, y) != after.pixel(x, y) ? 1 : 0;
        }
    }
    CHECK(differing > 200);
}

// _draw_live_listening (waveform_overlay.py:382-395): the pill's edge flashes for 0.6 s as a
// recording starts; Command Mode, which keeps its accent ring, does not flash.
TEST_CASE("a recording flashes the pill's edge as it starts and a command does not", "[widget]") {
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("bars"));
    // The top edge's middle, on the flash's outline and clear of the look and its label.
    const auto edgeAt = [&overlay](double elapsed) {
        overlay.access.listenStarted() = overlay.access.now() - elapsed;
        return overlay->grab().toImage().pixelColor(overlay->width() / 2, 1);
    };

    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->timer().stop();
    const QColor settled = edgeAt(5.0);
    CHECK(edgeAt(0.1) != settled);
    CHECK(edgeAt(0.7) == settled);

    overlay->hide();
    overlay->showAtCursor(WaveformOverlay::kStateCommandListening);
    overlay->timer().stop();
    CHECK(edgeAt(0.1) == edgeAt(5.0));
    CHECK_FALSE(overlay.access.paintErrorLogged());
}

// from tests/test_recording_look.py::test_refresh_reads_the_saved_look_and_label
TEST_CASE("refresh reads the saved look and label", "[widget][test_recording_look]") {
    fv::MemorySettingsStore settings;
    auto clock = fvtest::pinnedClock();
    WaveformOverlay overlay(&settings, clock.get());
    settings.update(
        fv::Json{{"recording_overlay_look", "fish"}, {"recording_overlay_clock", false}});
    overlay.refreshRecordingLook();
    CHECK(overlay.recordingLook() == QStringLiteral("fish"));
    CHECK(overlay.labelParts() == WaveformOverlay::LabelParts{true, true, false});
}

// from tests/test_recording_look.py::test_any_mix_of_label_parts_paints
TEST_CASE("any mix of label parts paints", "[widget][test_recording_look]") {
    const auto [dot, text, clock] = GENERATE(table<bool, bool, bool>({
        {false, true, true},
        {true, false, true},
        {true, true, false},
        {false, false, true},
        {false, false, false},
    }));
    CAPTURE(dot, text, clock);
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("ribbon"));
    overlay->setLabelParts(dot, text, clock);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::run(overlay, 0.5, kSpeech);
}

// from tests/test_recording_look.py::test_every_look_finishes_through_processing_and_transcribing
TEST_CASE("every look finishes through processing and transcribing",
          "[widget][test_recording_look]") {
    const std::string theme = GENERATE("dark", "light");
    const QString look = anyLook();
    CAPTURE(theme, look.toStdString());
    fvtest::ScopedPalette palette(themeFor(theme));
    Overlay overlay;
    finish(overlay, look);
    fvtest::run(overlay, 0.7, 0.0f);
    overlay->setState(WaveformOverlay::kStateTranscribing);
    fvtest::run(overlay, 0.5, 0.0f);
    overlay->hide();
    fvtest::run(overlay, 0.1, 0.0f);
}

// from tests/test_recording_look.py::test_transcribing_speeds_the_finish_up_without_a_jump
TEST_CASE("transcribing speeds the finish up without a jump", "[widget][test_recording_look]") {
    Overlay overlay;
    finish(overlay, QStringLiteral("ribbon"));
    fvtest::run(overlay, 0.5, 0.0f);
    CHECK(overlay.access.workRate() == Approx(1.0));
    const double before = overlay.access.workClock();
    overlay->setState(WaveformOverlay::kStateTranscribing);
    // The clock carries on from where it was.
    CHECK(overlay.access.workClock() == before);
    fvtest::run(overlay, 0.1, 0.0f);
    // The rate eases up rather than jumping.
    CHECK(1.0 < overlay.access.workRate());
    CHECK(overlay.access.workRate() < 1.7);
    fvtest::run(overlay, 1.0, 0.0f);
    CHECK(overlay.access.workRate() == Approx(1.7).margin(0.05));
}

// from tests/test_recording_look.py::test_the_stage_label_crossfades
TEST_CASE("the stage label crossfades", "[widget][test_recording_look]") {
    Overlay overlay;
    finish(overlay, QStringLiteral("bars"));
    CHECK(overlay.access.labelFrom().isEmpty());
    overlay->setState(WaveformOverlay::kStateTranscribing);
    CHECK(overlay.access.labelFrom() == QStringLiteral("Processing..."));
    CHECK(overlay.access.now() - overlay.access.labelChanged() < 0.1);
}

// from tests/test_recording_look.py::test_cleaning_up_takes_over_from_every_looks_finish
TEST_CASE("cleaning up takes over from every looks finish", "[widget][test_recording_look]") {
    const std::string theme = GENERATE("dark", "light");
    const QString look = anyLook();
    CAPTURE(theme, look.toStdString());
    fvtest::ScopedPalette palette(themeFor(theme));
    Overlay overlay;
    finish(overlay, look);
    overlay->setState(WaveformOverlay::kStateTranscribing);
    fvtest::run(overlay, 0.3, 0.0f);
    overlay->setState(WaveformOverlay::kStateCleaning);
    // The finish and its label hand off.
    CHECK(overlay.access.labelFrom() == QStringLiteral("Transcribing..."));
    fvtest::run(overlay, fv::kTidyPeriodS, 0.0f); // a whole pass of the caret
    overlay->hide();
    fvtest::run(overlay, 0.1, 0.0f);
}

// from tests/test_recording_look.py::test_rewriting_after_a_command_has_nothing_to_hand_off
TEST_CASE("rewriting after a command has nothing to hand off", "[widget][test_recording_look]") {
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("bars"));
    overlay->showAtCursor(WaveformOverlay::kStateCommandListening);
    overlay->setState(WaveformOverlay::kStateRewriting);
    CHECK(overlay.access.labelFrom().isEmpty());
    fvtest::run(overlay, 0.5, 0.0f);
}

// from tests/test_recording_look.py::test_a_finished_result_fades_out_then_hides
TEST_CASE("a finished result fades out then hides", "[widget][test_recording_look]") {
    Overlay overlay;
    finish(overlay, QStringLiteral("aurora"));
    overlay->hide();
    // It fades first.
    CHECK(overlay->isVisible());
    fvtest::run(overlay, 0.1, 0.0f);
    CHECK(overlay->isVisible());
    // The fade runs on the wall clock.
    REQUIRE(overlay.access.fadingSince().has_value());
    *overlay.access.fadingSince() -= WaveformOverlay::kFadeOutS;
    fvtest::run(overlay, 0.1, 0.0f);
    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateIdle);
}

// from tests/test_recording_look.py::test_a_new_recording_cancels_the_fade
TEST_CASE("a new recording cancels the fade", "[widget][test_recording_look]") {
    Overlay overlay;
    finish(overlay, QStringLiteral("dots"));
    overlay->hide();
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::run(overlay, WaveformOverlay::kFadeOutS + 0.1, kSpeech);
    CHECK(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateRecording);
}

// from tests/test_recording_look.py::test_states_that_are_not_work_hide_at_once
TEST_CASE("states that are not work hide at once", "[widget][test_recording_look]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->hide();
    CHECK_FALSE(overlay->isVisible());
}

// from tests/test_recording_look.py::test_classic_finishes_with_the_shared_vortex
TEST_CASE("classic finishes with the shared vortex", "[widget][test_recording_look]") {
    Overlay overlay;
    finish(overlay, QStringLiteral("legacy"));
    CHECK(overlay.access.visual() == nullptr);
    fvtest::run(overlay, 0.5, 0.0f);
    // The vortex is spinning.
    CHECK_FALSE(overlay.access.style().particles().empty());
}
