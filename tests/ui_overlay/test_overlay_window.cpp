// The overlay window's focus safety, hands-free badge, captions and command states
// (tests/test_personalize_s8_overlay.py), and the overlay half of the shell's new overlay states
// (tests/test_personalize_foundation_ui_shell.py).
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QProcess>
#include <QProcessEnvironment>
#include <QString>

#include "appkit/app_context.h"
#include "overlay_test_support.h"
#include "settings/testing/memory_store.h"
#include "ui_overlay/overlay_feature.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

// from tests/test_personalize_s8_overlay.py::TestFocus::test_standalone_overlay_never_takes_focus
TEST_CASE("standalone overlay never takes focus", "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    const Qt::WindowFlags flags = overlay->windowFlags();
    CHECK(flags.testFlag(Qt::WindowDoesNotAcceptFocus));
    CHECK((flags & Qt::Tool) == Qt::Tool);
    CHECK(overlay->testAttribute(Qt::WA_ShowWithoutActivating));
    CHECK(overlay->focusPolicy() == Qt::NoFocus);
}

// from
// tests/test_personalize_s8_overlay.py::TestFocus::test_native_window_is_non_activating_on_windows
TEST_CASE("native window is non activating on windows", "[platform][test_personalize_s8_overlay]") {
#ifndef FV_NOACTIVATE_HELPER
    SKIP("WS_EX_NOACTIVATE is a Windows window style");
#else
    // Qt's windows platform, which this offscreen executable cannot load, makes the overlay's
    // native window in another process; it is never shown.
    QProcess helper;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("windows"));
    helper.setProcessEnvironment(environment);
    helper.start(QStringLiteral(FV_NOACTIVATE_HELPER), {});
    REQUIRE(helper.waitForFinished(60'000));
    const QString output = QString::fromLocal8Bit(helper.readAllStandardOutput()).trimmed();
    INFO(QString::fromLocal8Bit(helper.readAllStandardError()).toStdString());
    REQUIRE(helper.exitStatus() == QProcess::NormalExit);
    REQUIRE(helper.exitCode() == 0);
    CHECK(output.split(u'\n').last().trimmed() == QString::number(0x08000000));
#endif
}

// from
// tests/test_personalize_s8_overlay.py::TestLanguageChip::test_dictating_right_after_a_notice_keeps_the_overlay
TEST_CASE("dictating right after a notice keeps the overlay",
          "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->setLanguage(QStringLiteral("es"), {QStringLiteral("en"), QStringLiteral("es")});
    overlay->showLanguageNotice();
    CHECK(overlay.access.hiddenTimer().isActive());
    overlay->setState(WaveformOverlay::kStateRecording);
    CHECK_FALSE(overlay.access.hiddenTimer().isActive());
    CHECK(overlay->isVisible());
}

// from tests/test_personalize_s8_overlay.py::TestHandsFree::test_latch_lasts_for_the_recording
TEST_CASE("latch lasts for the recording", "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setHandsFree(true);
    CHECK(overlay->handsFree());
    overlay->setState(WaveformOverlay::kStateStreaming);
    CHECK(overlay->handsFree());
    fvtest::paint(overlay);
    overlay->setState(WaveformOverlay::kStateProcessing);
    CHECK_FALSE(overlay->handsFree());
}

// from tests/test_personalize_s8_overlay.py::TestHandsFree::test_hide_and_a_new_recording_clear_it
TEST_CASE("hide and a new recording clear it", "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setHandsFree(true);
    overlay->hide();
    CHECK_FALSE(overlay->handsFree());

    overlay->setHandsFree(true);
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK_FALSE(overlay->handsFree());
}

// from tests/test_personalize_s8_overlay.py::TestHandsFree::test_latch_is_remembered_while_hidden
TEST_CASE("latch is remembered while hidden", "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->setHandsFree(true);
    CHECK(overlay->handsFree());
    CHECK_FALSE(overlay->isVisible());
}

// from tests/test_personalize_s8_overlay.py::TestCaption::test_caption_shows_for_a_moment
TEST_CASE("caption shows for a moment", "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->showCaption(QStringLiteral("Switched to Laptop mic"));
    CHECK(overlay.access.caption() == QStringLiteral("Switched to Laptop mic"));
    CHECK(overlay.access.captionTimer().isActive());
    CHECK(overlay.access.captionTimer().interval() == WaveformOverlay::kCaptionMs);
    fvtest::paint(overlay);
    fvtest::fireTimeout(overlay.access.captionTimer());
    CHECK(overlay.access.caption().isEmpty());
}

// from
// tests/test_personalize_s8_overlay.py::TestCaption::test_caption_is_cleared_by_hide_and_by_the_recording_ending
TEST_CASE("caption is cleared by hide and by the recording ending",
          "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->showCaption(QStringLiteral("Switched to Laptop mic"));
    overlay->hide();
    CHECK(overlay.access.caption().isEmpty());
    CHECK_FALSE(overlay.access.captionTimer().isActive());

    overlay->showAtCursor(WaveformOverlay::kStateStreaming);
    overlay->showCaption(QStringLiteral("Switched to Laptop mic"));
    overlay->setState(WaveformOverlay::kStateTranscribing);
    CHECK(overlay.access.caption().isEmpty());
}

// from tests/test_personalize_s8_overlay.py::TestCaption::test_hidden_overlay_ignores_captions
TEST_CASE("hidden overlay ignores captions", "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->showCaption(QStringLiteral("Switched to Laptop mic"));
    CHECK(overlay.access.caption().isEmpty());
    CHECK_FALSE(overlay->isVisible());
}

// from
// tests/test_personalize_s8_overlay.py::TestCommandStates::test_command_listening_records_with_particles_and_a_preview
TEST_CASE("command listening records with particles and a preview",
          "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateCommandListening);
    overlay->updateAudioLevels(QList<float>(20, 0.6f));
    for (int i = 0; i < 5; ++i) {
        overlay.access.lastFrameTime() -= 0.05;
        overlay.access.updateAnimation();
    }
    CHECK_FALSE(overlay.access.style().particles().empty());
    fvtest::paint(overlay);

    QString words;
    for (int i = 0; i < 4; ++i) {
        words += QStringLiteral("make this friendlier and shorter please ");
    }
    overlay->updateStreamingText(words);
    CHECK(overlay->height() > overlay.access.baseHeight());
    fvtest::paint(overlay);
    overlay->setState(WaveformOverlay::kStateRewriting);
    CHECK(overlay->height() == overlay.access.baseHeight());
    CHECK(overlay->streamingText().isEmpty());
}

// from
// tests/test_personalize_s8_overlay.py::TestCommandStates::test_rewriting_shimmers_like_cleanup
TEST_CASE("rewriting shimmers like cleanup", "[widget][test_personalize_s8_overlay]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRewriting);
    overlay.access.updateAnimation();
    CHECK(overlay->timer().isActive());
    fvtest::paint(overlay);
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestAppWiring::test_command_listening_records_and_rewriting_cleans_up_in_their_own_looks
TEST_CASE("command listening records and rewriting cleans up in their own looks",
          "[widget][test_personalize_foundation_ui_shell]") {
    // The overlay's side of the controller's set_overlay_state(COMMAND_LISTENING, REWRITING,
    // NONE); the tray and tab labels are the controller's tests.
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateCommandListening);
    CHECK(overlay->isVisible());
    CHECK(overlay->currentState() == WaveformOverlay::kStateCommandListening);

    overlay->setState(WaveformOverlay::kStateRewriting);
    CHECK(overlay->currentState() == WaveformOverlay::kStateRewriting);

    overlay->hide();
    // A finished rewrite fades out first.
    CHECK(overlay->isVisible());
    REQUIRE(overlay.access.fadingSince().has_value());
    *overlay.access.fadingSince() -= WaveformOverlay::kFadeOutS;
    overlay.access.updateAnimation();
    CHECK_FALSE(overlay->isVisible());
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestAppWiring::test_new_overlay_hooks_change_nothing_yet
TEST_CASE("new overlay hooks change nothing yet",
          "[widget][test_personalize_foundation_ui_shell]") {
    Overlay overlay;
    const QString state = overlay->currentState();
    overlay->setHandsFree(true);
    overlay->showCaption(QStringLiteral("Switched to Laptop mic"));
    CHECK_FALSE(overlay->isVisible());
    CHECK(overlay->currentState() == state);
}

// _restart_animation and _update_animation (waveform_overlay.py:1316-1427): 1000 // 30 ms
// frames while the state is not idle, and a frame never steps more than 0.1 s however long the
// last one took (a stall, a sleeping machine).
TEST_CASE("the frame timer runs only while not idle and a stalled frame steps 0.1 s", "[widget]") {
    Overlay overlay;
    CHECK_FALSE(overlay->timer().isActive());
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    CHECK(overlay->timer().isActive());
    CHECK(overlay->timer().interval() == 33);

    overlay->timer().stop();
    overlay.access.lastFrameTime() = overlay.access.now() - 5.0;
    overlay.access.updateAnimation();
    CHECK(overlay.access.animationTime() == 0.1);
    // A clock that went backwards steps nothing.
    overlay.access.lastFrameTime() = overlay.access.now() + 1.0;
    overlay.access.updateAnimation();
    CHECK(overlay.access.animationTime() == 0.1);

    overlay->setState(WaveformOverlay::kStateIdle);
    CHECK_FALSE(overlay->timer().isActive());
}

// set_state's keeps_destination and _finish_hide (waveform_overlay.py:1376-1379, 1678): the
// capture's destination stays through its work and cleanup, and goes with Command Mode, a
// moment or the overlay hiding.
TEST_CASE("the destination badge lasts through the job and goes when it ends", "[widget]") {
    Overlay overlay;
    overlay->showAtCursor(WaveformOverlay::kStateRecording);
    overlay->setRecordingDestination(QStringLiteral("Scratchpad"));
    // The look sits under the badge row.
    CHECK(overlay->height() == overlay.access.baseHeight() + 27);
    fvtest::paint(overlay);
    for (const QString& state :
         {WaveformOverlay::kStateProcessing, WaveformOverlay::kStateTranscribing,
          WaveformOverlay::kStateCleaning}) {
        overlay->setState(state);
        CHECK(overlay->recordingDestination() == QStringLiteral("Scratchpad"));
        fvtest::paint(overlay);
    }
    overlay->setState(WaveformOverlay::kStateCopied);
    CHECK(overlay->recordingDestination().isEmpty());
    CHECK(overlay->height() == overlay.access.baseHeight());

    overlay->setState(WaveformOverlay::kStateRecording);
    overlay->setRecordingDestination(QStringLiteral("Scratchpad"));
    overlay->setState(WaveformOverlay::kStateCommandListening);
    CHECK(overlay->recordingDestination().isEmpty());

    overlay->setState(WaveformOverlay::kStateRecording);
    overlay->setRecordingDestination(QStringLiteral("Scratchpad"));
    overlay->hide();
    CHECK(overlay->recordingDestination().isEmpty());
    CHECK(overlay->height() == overlay.access.baseHeight());
}

TEST_CASE("the overlay feature provides one top-level overlay, hidden", "[widget]") {
    fv::MemorySettingsStore settings;
    auto clock = fvtest::pinnedClock();
    fv::AppServices services;
    services.settings = &settings;
    services.clock = clock.get();
    fv::AppContext context(services);
    fv::OverlayFeature feature;
    CHECK(feature.id() == "overlay");
    feature.provide(context);
    feature.install(context);
    auto& overlay = context.service<WaveformOverlay>();
    CHECK(overlay.parentWidget() == nullptr);
    CHECK(overlay.isWindow());
    CHECK_FALSE(overlay.isVisible());
    CHECK(overlay.currentState() == WaveformOverlay::kStateIdle);
    CHECK(overlay.recordingLook() == QStringLiteral("bars"));
}

TEST_CASE("the overlay reads its look and label from the settings it was given", "[widget]") {
    fv::MemorySettingsStore settings;
    settings.update(
        fv::Json{{"recording_overlay_look", "legacy"}, {"recording_overlay_text", false}});
    WaveformOverlay overlay(&settings);
    CHECK(overlay.recordingLook() == QStringLiteral("legacy"));
    CHECK(overlay.labelParts() == WaveformOverlay::LabelParts{true, false, true});
}
