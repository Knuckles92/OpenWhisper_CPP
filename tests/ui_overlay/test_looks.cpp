// The recording looks on the overlay (tests/test_recording_look.py): each look's own motion as
// the overlay drives it, frame by frame, with the voice the tests use.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "overlay_test_support.h"
#include "settings/enums.h"
#include "ui_overlay/ocean_looks.h"
#include "ui_overlay/pulse_look.h"
#include "uimath/overlay_look.h"
#include "uimath/voice_ribbon.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

// A word on a desk microphone, as raw RMS, and a quiet room.
constexpr float kSpeech = 0.06f;
constexpr float kSilence = 0.002f;

std::set<std::string> entryValues(const std::vector<fv::enums::Entry>& entries) {
    std::set<std::string> values;
    for (const fv::enums::Entry& entry : entries) {
        values.insert(entry.value.get<std::string>());
    }
    return values;
}

const fv::SchoolLook& school(Overlay& overlay) {
    const auto* look = dynamic_cast<const fv::SchoolLook*>(overlay.access.visual());
    REQUIRE(look != nullptr);
    return *look;
}

const fv::FizzLook& fizz(Overlay& overlay) {
    const auto* look = dynamic_cast<const fv::FizzLook*>(overlay.access.visual());
    REQUIRE(look != nullptr);
    return *look;
}

double sum(const fv::FizzSim::Levels& levels) {
    double total = 0.0;
    for (const double level : levels) {
        total += level;
    }
    return total;
}

} // namespace

// from tests/test_recording_look.py::test_settings_and_the_overlay_agree_on_the_looks
TEST_CASE("settings and the overlay agree on the looks", "[test_recording_look]") {
    using fv::enums::EnumId;
    const fv::Json& all = fv::enums::values(EnumId::RecordingOverlayLook);
    std::vector<std::string> saved;
    for (const fv::Json& value : all) {
        saved.push_back(value.get<std::string>());
    }
    const std::vector<std::string> looks(fv::kLooks.begin(), fv::kLooks.end());
    CHECK(saved == looks);
    const std::set<std::string> lookSet(looks.begin(), looks.end());
    CHECK(entryValues(fv::enums::labels(EnumId::RecordingOverlayLook)) == lookSet);
    CHECK(entryValues(fv::enums::details(EnumId::RecordingOverlayLook)) == lookSet);
}

// from tests/test_recording_look.py::test_live_particles_leave_from_the_ribbon
TEST_CASE("live particles leave from the ribbon", "[widget][test_recording_look]") {
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("ribbon"));
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::run(overlay, 1.0, kSpeech);
    const double baseline = overlay.access.baseHeight() - fv::kBaselineFromBottom;
    REQUIRE(overlay.access.style().emitter);
    CHECK(overlay.access.style().emitter(overlay->width() - 20) < baseline - 5);
}

// from tests/test_recording_look.py::test_pulse_ripples_when_a_word_starts
TEST_CASE("pulse ripples when a word starts", "[widget][test_recording_look]") {
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("pulse"));
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    const auto* pulse = dynamic_cast<const fv::PulseLook*>(overlay.access.visual());
    REQUIRE(pulse != nullptr);
    fvtest::run(overlay, 0.4, kSilence);
    CHECK(pulse->sim().ripples().empty());
    fvtest::run(overlay, 0.2, kSpeech);
    CHECK_FALSE(pulse->sim().ripples().empty());
}

// from tests/test_recording_look.py::test_the_school_heaves_the_swell_with_the_voice
TEST_CASE("the school heaves the swell with the voice", "[widget][test_recording_look]") {
    const QString look =
        GENERATE(QStringLiteral("baitball"), QStringLiteral("fish"), QStringLiteral("glow"));
    CAPTURE(look.toStdString());
    Overlay overlay;
    overlay->setRecordingLook(look);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    const auto bottom = [&overlay] {
        const auto& layer = school(overlay).sim().displacement()[0];
        return *std::max_element(layer.begin(), layer.end());
    };
    fvtest::run(overlay, 1.5, kSilence);
    const double calm = bottom();
    fvtest::run(overlay, 1.5, kSpeech);
    // The school surges up and pushes the bottom swell.
    CHECK(bottom() > calm + 3);
    // Every layer stays above the one beneath it.
    const fv::SchoolSim& sim = school(overlay).sim();
    for (int p = 0; p < fv::SchoolSim::kPoints; ++p) {
        for (int layer = 1; layer < fv::SchoolSim::kLayers; ++layer) {
            CHECK(sim.y_of(layer, p) <= sim.y_of(layer - 1, p) - fv::SchoolSim::kGapMin + 0.5);
        }
    }
}

// from tests/test_recording_look.py::test_the_bait_ball_rises_while_talking_and_sinks_in_pauses
TEST_CASE("the bait ball rises while talking and sinks in pauses",
          "[widget][test_recording_look]") {
    // Real speech is words with gaps; the ball must follow phrases, not average them away.
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("baitball"));
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    const auto height = [&overlay] {
        const fv::SchoolSim& sim = school(overlay).sim();
        double total = 0.0;
        for (const fv::Fish& f : sim.fish()) {
            total += f.y;
        }
        return fv::SchoolSim::kBase - total / static_cast<double>(sim.fish().size());
    };
    fvtest::run(overlay, 1.0, kSilence);
    std::vector<double> talking;
    std::vector<double> resting;
    for (int phrase = 0; phrase < 2; ++phrase) {
        for (int word = 0; word < 6; ++word) { // a phrase: words with short gaps
            fvtest::run(overlay, 0.18, kSpeech);
            fvtest::run(overlay, 0.07, kSilence);
            if (word >= 2) {
                talking.push_back(height());
            }
        }
        for (int beat = 0; beat < 9; ++beat) { // a pause
            fvtest::run(overlay, 0.1, kSilence);
            if (beat >= 5) {
                resting.push_back(height());
            }
        }
    }
    const auto mean = [](const std::vector<double>& values) {
        double total = 0.0;
        for (const double value : values) {
            total += value;
        }
        return total / static_cast<double>(values.size());
    };
    const double swing = mean(talking) - mean(resting);
    INFO("talking should lift the ball well above where it rests (swing " << swing << " px)");
    CHECK(swing > 12);
}

// from tests/test_recording_look.py::test_fizz_fills_further_for_a_louder_voice
TEST_CASE("fizz fills further for a louder voice", "[widget][test_recording_look]") {
    Overlay overlay;
    std::vector<double> fills;
    for (const float level : {0.015f, 0.06f}) { // quiet speech, then ordinary speech
        overlay->setRecordingLook(QStringLiteral("fizz"));
        overlay->showAtCursor(WaveformOverlay::kStateRecording);
        fvtest::run(overlay, 1.5, level);
        const fv::FizzSim& sim = fizz(overlay).sim();
        fills.push_back(sum(sim.levels()) / fv::FizzSim::kLanes / fv::FizzSim::kMaxHeight);
        overlay->hide();
    }
    // The level shows how loud, not just that someone spoke.
    CHECK(fills[0] < 0.45);
    CHECK(fills[1] > 0.6);
}

// from tests/test_recording_look.py::test_fizz_fills_with_the_voice_and_drains_in_silence
TEST_CASE("fizz fills with the voice and drains in silence", "[widget][test_recording_look]") {
    Overlay overlay;
    overlay->setRecordingLook(QStringLiteral("fizz"));
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    fvtest::run(overlay, 1.5, kSpeech);
    const double full = sum(fizz(overlay).sim().levels());
    fvtest::run(overlay, 1.5, kSilence);
    CHECK(sum(fizz(overlay).sim().levels()) < full / 2);
}

// recording_looks.make_visual: every look but Classic has a visual of its own, with the
// particles, small drawing and finish Python gives it.
TEST_CASE("every look but classic has its own visual", "[widget]") {
    struct Expect {
        const char* look;
        bool particles;
        std::string_view small;
    };
    const Expect expect =
        GENERATE(Expect{"ribbon", true, fv::kSmallSquash}, Expect{"bars", false, fv::kSmallSquash},
                 Expect{"wave", true, fv::kSmallSquash}, Expect{"aurora", false, fv::kSmallSquash},
                 Expect{"pulse", false, fv::kSmallShrink}, Expect{"dots", false, fv::kSmallShrink},
                 Expect{"baitball", false, fv::kSmallFit}, Expect{"fish", false, fv::kSmallFit},
                 Expect{"glow", false, fv::kSmallFit}, Expect{"fizz", false, fv::kSmallFit});
    CAPTURE(expect.look);
    Overlay overlay;
    overlay->setRecordingLook(QString::fromLatin1(expect.look));
    const fv::Visual* visual = overlay.access.visual();
    REQUIRE(visual != nullptr);
    CHECK(visual->particles() == expect.particles);
    CHECK(visual->small() == expect.small);
    // The particles launch from the look where it has them, else from the flat line.
    CHECK(static_cast<bool>(overlay.access.style().emitter) == expect.particles);
    overlay->setRecordingLook(QStringLiteral("legacy"));
    CHECK(overlay.access.visual() == nullptr);
}
