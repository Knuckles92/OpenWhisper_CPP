// The overlay against the frames OpenWhisper rendered for it (tools/parity/builders/a07.py,
// tests/reference/waveform_overlay/<scene>/<theme>-windows11-dpr1-s100.{png,json}). Each scene
// is built as the builder builds it: the clock pinned, the frame's moments set, the ribbon fed
// the same levels. Scenes that play random confetti, bubbles or particles compare the tree only.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <functional>
#include <string>

#include <QApplication>

#include "overlay_test_support.h"
#include "screenshot/screenshot.h"
#include "ui_overlay/recording_look_preview.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "uimath/sample_voice.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

// The builder's NOW, HUE_TIME and VOICE.
constexpr double kNow = 1000.0;
constexpr double kHueTime = 3.0;
constexpr std::array<double, 22> kVoice = {0.002, 0.004, 0.02, 0.06, 0.09, 0.05, 0.03,  0.008,
                                           0.002, 0.03,  0.07, 0.12, 0.08, 0.04, 0.015, 0.003,
                                           0.002, 0.05,  0.1,  0.06, 0.02, 0.004};

// The application styled the way OpenWhisper's QtApplication styles it, for one test case.
class AppTheme {
public:
    explicit AppTheme(const QString& theme) : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(100, *qApp, manager_);
    }
    ~AppTheme() { qApp->setStyleSheet(QString()); }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

void pinLook(Overlay& overlay, const QString& look = QStringLiteral("bars")) {
    REQUIRE(overlay.access.now() == kNow);
    overlay->setRecordingLook(look);
    overlay.access.style().set_animation_time(kHueTime);
}

void voice(Overlay& overlay) {
    for (int pass = 0; pass < 3; ++pass) {
        for (const double level : kVoice) {
            overlay.access.ribbon().hear(level);
            overlay.access.ribbon().advance(1.0 / 30);
        }
    }
}

void recording(Overlay& overlay) {
    pinLook(overlay);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    voice(overlay);
    overlay.access.listenStarted() = kNow - 4.4;
}

void shown(Overlay& overlay, const QString& state, double at,
           const QString& look = QStringLiteral("bars")) {
    pinLook(overlay, look);
    overlay->showAtCursor(state);
    overlay.access.animationTime() = at;
}

void burst(Overlay& overlay, const QString& state, double at) {
    shown(overlay, state, 0.0);
    overlay.access.animationTime() = at;
    overlay.access.updateAnimation();
}

using Scene = std::function<void(Overlay&)>;

void still(Overlay& overlay) {
    overlay->timer().stop();
    overlay.access.hiddenTimer().stop();
    overlay.access.captionTimer().stop();
}

void check(const std::string& scene, const std::string& theme, const Scene& build) {
    AppTheme appTheme(QString::fromStdString(theme));
    Overlay overlay;
    build(overlay);
    still(overlay);
    fv::screenshot::expect_matches(*overlay, "waveform_overlay/" + scene,
                                   theme + "-windows11-dpr1-s100");
}

void checkTree(const std::string& scene, const std::string& theme, const Scene& build) {
    AppTheme appTheme(QString::fromStdString(theme));
    Overlay overlay;
    build(overlay);
    still(overlay);
    const fv::Json pins =
        fv::screenshot::load_reference("waveform_overlay/" + scene, theme + "-windows11-dpr1-s100");
    for (const auto& difference :
         fv::screenshot::compare_trees(pins.at("tree"), fv::screenshot::dump_tree(*overlay))) {
        FAIL_CHECK(scene << "/" << theme << " tree: " << difference);
    }
}

} // namespace

#define OVERLAY_SCENE(scene, ...)                                                                  \
    TEST_CASE("reference screenshot waveform_overlay " scene " dark", "[screenshot]") {            \
        check(scene, "dark", __VA_ARGS__);                                                         \
    }                                                                                              \
    TEST_CASE("reference screenshot waveform_overlay " scene " light", "[screenshot]") {           \
        check(scene, "light", __VA_ARGS__);                                                        \
    }

#define OVERLAY_TREE(scene, ...)                                                                   \
    TEST_CASE("reference tree waveform_overlay " scene " dark", "[widget]") {                      \
        checkTree(scene, "dark", __VA_ARGS__);                                                     \
    }                                                                                              \
    TEST_CASE("reference tree waveform_overlay " scene " light", "[widget]") {                     \
        checkTree(scene, "light", __VA_ARGS__);                                                    \
    }

OVERLAY_SCENE("recording-bars", [](Overlay& overlay) { recording(overlay); })

OVERLAY_SCENE("recording-bars-handsfree", [](Overlay& overlay) {
    recording(overlay);
    overlay->setHandsFree(true);
})

OVERLAY_SCENE("recording-bars-caption", [](Overlay& overlay) {
    recording(overlay);
    overlay->showCaption(QStringLiteral("Switched to Laptop mic"));
    overlay.access.captionStarted() = kNow - 1.0;
})

OVERLAY_SCENE("processing-bars", [](Overlay& overlay) {
    recording(overlay);
    overlay->setState(WaveformOverlay::kStateProcessing);
    overlay.access.workStarted() = kNow - 0.6;
    overlay.access.workClock() = 0.6;
    overlay.access.animationTime() = 0.6;
})

OVERLAY_SCENE("transcribing-bars", [](Overlay& overlay) {
    recording(overlay);
    overlay->setState(WaveformOverlay::kStateProcessing);
    overlay->setState(WaveformOverlay::kStateTranscribing);
    overlay.access.workStarted() = kNow - 1.4;
    overlay.access.labelChanged() = kNow - 0.8;
    overlay.access.workClock() = 1.4;
    overlay.access.animationTime() = 0.8;
})

OVERLAY_SCENE("cleaning",
              [](Overlay& overlay) { shown(overlay, WaveformOverlay::kStateCleaning, 1.0); })

OVERLAY_SCENE("canceling",
              [](Overlay& overlay) { shown(overlay, WaveformOverlay::kStateCanceling, 0.10); })

OVERLAY_SCENE("stt-enable",
              [](Overlay& overlay) { shown(overlay, WaveformOverlay::kStateSttEnable, 0.12); })

OVERLAY_SCENE("copied",
              [](Overlay& overlay) { shown(overlay, WaveformOverlay::kStateCopied, 0.12); })

OVERLAY_SCENE("notice-problem", [](Overlay& overlay) {
    pinLook(overlay);
    overlay->showNotice(QStringLiteral("Transcription failed. Retry the recording from History."));
    overlay.access.animationTime() = 0.6;
})

// Classic before its first tick: no particle yet, so its pixels are fixed.
OVERLAY_SCENE("recording-classic", [](Overlay& overlay) {
    pinLook(overlay, QStringLiteral("legacy"));
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
})

OVERLAY_TREE("recording-classic-particles", [](Overlay& overlay) {
    pinLook(overlay, QStringLiteral("legacy"));
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->updateAudioLevels(QList<float>(20, 0.06f));
    for (int i = 0; i < 15; ++i) {
        overlay.access.lastFrameTime() = kNow - 1.0 / 30;
        overlay.access.updateAnimation();
    }
    CHECK_FALSE(overlay.access.style().particles().empty());
})

OVERLAY_TREE("copied-confetti", [](Overlay& overlay) {
    burst(overlay, WaveformOverlay::kStateCopied, 0.6);
    CHECK_FALSE(overlay.access.sttParticles().empty());
})

OVERLAY_TREE("canceling-bubbles", [](Overlay& overlay) {
    burst(overlay, WaveformOverlay::kStateCanceling, 0.4);
    CHECK(overlay.access.sttParticles().size() == 72);
})

// --- the other looks, the live preview, the language notice and the split -------------------

namespace {

// The builder's WORDS, LOOK_T and PREVIEW_T.
const QString kWords = QStringLiteral("Hello from OpenWhisper_CPP, this is the live preview");
constexpr double kLookT = 1.2;
constexpr double kPreviewT = 2.0;

// _recording_look: LOOK_T seconds of the fixed voice, each frame through the look's motion.
void recordingLook(Overlay& overlay, const QString& look) {
    pinLook(overlay, look);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    REQUIRE(overlay.access.visual() != nullptr);
    const int frames = static_cast<int>(std::lround(kLookT * 30));
    for (int frame = 0; frame < frames; ++frame) {
        overlay.access.ribbon().hear(kVoice[static_cast<std::size_t>(frame) % kVoice.size()]);
        overlay.access.ribbon().advance(1.0 / 30);
        overlay.access.visual()->advance(1.0 / 30);
    }
    overlay.access.listenStarted() = kNow - kLookT;
}

void streaming(Overlay& overlay, fv::PreviewLayout layout, bool chip = false) {
    pinLook(overlay);
    overlay.access.streamingLayout() = layout;
    REQUIRE(overlay->streamingFontSize() == 16);
    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    voice(overlay);
    overlay.access.listenStarted() = kNow - 4.4;
    if (chip) {
        overlay->setLanguage(QStringLiteral("en"), {QStringLiteral("en"), QStringLiteral("es")});
    }
    overlay->updateStreamingText(kWords);
}

} // namespace

OVERLAY_SCENE("recording-ribbon",
              [](Overlay& overlay) { recordingLook(overlay, QStringLiteral("ribbon")); })

OVERLAY_SCENE("recording-wave",
              [](Overlay& overlay) { recordingLook(overlay, QStringLiteral("wave")); })

OVERLAY_SCENE("recording-aurora",
              [](Overlay& overlay) { recordingLook(overlay, QStringLiteral("aurora")); })

OVERLAY_SCENE("recording-pulse",
              [](Overlay& overlay) { recordingLook(overlay, QStringLiteral("pulse")); })

OVERLAY_SCENE("recording-dots",
              [](Overlay& overlay) { recordingLook(overlay, QStringLiteral("dots")); })

OVERLAY_SCENE("streaming-header",
              [](Overlay& overlay) { streaming(overlay, fv::PreviewLayout::Header); })

OVERLAY_SCENE("streaming-footer",
              [](Overlay& overlay) { streaming(overlay, fv::PreviewLayout::Footer); })

OVERLAY_SCENE("streaming-header-chip",
              [](Overlay& overlay) { streaming(overlay, fv::PreviewLayout::Header, true); })

OVERLAY_SCENE("language-notice", [](Overlay& overlay) {
    pinLook(overlay);
    overlay->setLanguage(QStringLiteral("en"), {QStringLiteral("en"), QStringLiteral("es")});
    overlay->showLanguageNotice();
    overlay.access.animationTime() = 0.4;
})

OVERLAY_SCENE("large-file", [](Overlay& overlay) {
    pinLook(overlay);
    overlay->setLargeFileInfo(25.4);
    overlay->showAtCursor(WaveformOverlay::kStateLargeFileSplitting);
    overlay.access.animationTime() = 0.7;
})

namespace {

// _walkthrough: the destination badge, Snap and Done, the clock at 0:12, on a platform that
// takes overlay clicks.
void walkthrough(Overlay& overlay, bool chip) {
    pinLook(overlay);
    overlay.access.setTakesClicks(true);
    overlay->setRecordingDestination(QStringLiteral("Walkthrough · 2 shots"));
    overlay->setWalkthroughControls(true);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    voice(overlay);
    overlay.access.listenStarted() = kNow - 12.4;
    if (chip) {
        overlay->setLanguage(QStringLiteral("en"), {QStringLiteral("en"), QStringLiteral("es")});
    }
}

} // namespace

OVERLAY_SCENE("walkthrough-recording", [](Overlay& overlay) { walkthrough(overlay, false); })

OVERLAY_SCENE("walkthrough-chip", [](Overlay& overlay) { walkthrough(overlay, true); })

namespace {

// recording_look_preview_bars: shown first, then held still, the sample voice fed frame by frame
// up to PREVIEW_T.
void checkPreview(const std::string& theme) {
    AppTheme appTheme(QString::fromStdString(theme));
    auto clock = fvtest::pinnedClock();
    fv::RecordingLookPreview preview({}, {}, nullptr, nullptr, clock.get());
    fv::WaveformOverlayTestAccess access(preview.overlay());
    REQUIRE(access.now() == kNow);
    preview.overlay().setRecordingLook(QStringLiteral("bars"));
    access.style().set_animation_time(kHueTime);
    preview.setFixedSize(560, preview.sizeHint().height());
    preview.show();
    preview.timer().stop();
    preview.overlay().timer().stop();
    access.hiddenTimer().stop();
    access.captionTimer().stop();
    const int frames = static_cast<int>(std::lround(kPreviewT * 30));
    for (int frame = 0; frame < frames; ++frame) {
        access.ribbon().hear(fv::sample_level(frame / 30.0));
        access.ribbon().advance(1.0 / 30);
    }
    access.listenStarted() = kNow - kPreviewT;
    fv::screenshot::expect_matches(preview, "recording_look_preview/bars",
                                   theme + "-windows11-dpr1-s100");
    preview.hide();
    preview.close();
}

} // namespace

TEST_CASE("reference screenshot recording_look_preview bars dark", "[screenshot]") {
    checkPreview("dark");
}

TEST_CASE("reference screenshot recording_look_preview bars light", "[screenshot]") {
    checkPreview("light");
}
