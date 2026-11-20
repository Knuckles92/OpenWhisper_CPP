// Shortcut and tray outcomes near the pointer (tests/test_personalize_fix3_h1_outcome_notices.py):
// what the controller sends to the overlay, and when it keeps quiet.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

#include <QPixmap>

#include "controller_support.h"
#include "history/manager.h"
#include "ui_controller/overlay_router.h"
#include "ui_history/history_actions.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;

namespace {

void saveDictation(ControllerRig& rig, const std::string& text,
                   const std::optional<std::string>& raw = std::nullopt) {
    auto& history = rig.context().service<fv::HistoryManager>();
    fvtest::offGui([&history, text, raw] {
        fv::AddEntry entry;
        entry.text = text;
        entry.model = "parakeet";
        entry.raw_text = raw;
        entry.source_name = "Quick Record";
        return history.add_entry(entry).has_value();
    });
}

// Every notice the controller asked the overlay for, in order.
struct NoticeLog {
    explicit NoticeLog(ControllerRig& rig) {
        link = rig.runtime().notice_requested.connect(
            rig.uiExecutor(),
            [this](std::string text, bool done) { shown.emplace_back(text, done); });
    }
    std::vector<std::pair<std::string, bool>> shown;
    fv::Subscription link;
};

} // namespace

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_recording_in_progress_keeps_its_overlay
// (through the controller: a notice while recording or transcribing leaves that overlay up)
TEST_CASE("test_a_recording_in_progress_keeps_its_overlay",
          "[test_personalize_fix3_h1_outcome_notices][widget]") {
    ControllerRig rig;
    fv::OverlayRouter& router = rig.ui().overlayRouter();

    router.setOverlayState(fv::appcore::OverlayState::Recording);
    rig.ui().showNotice(QStringLiteral("Nothing to paste yet"), false);
    CHECK(rig.overlay().currentState() == fv::WaveformOverlay::kStateRecording);

    router.setOverlayState(fv::appcore::OverlayState::Transcribing);
    rig.ui().showNotice(QStringLiteral("Nothing to paste yet"), false);
    CHECK(rig.overlay().currentState() == fv::WaveformOverlay::kStateTranscribing);
    CHECK_FALSE(rig.overlay().grab().isNull());
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_job_in_progress_keeps_its_overlay_and_shows_the_notice
TEST_CASE("test_a_job_in_progress_keeps_its_overlay_and_shows_the_notice",
          "[test_personalize_fix3_h1_outcome_notices][widget]") {
    using fv::appcore::OverlayState;
    const auto [state, name] =
        GENERATE(std::pair{OverlayState::Processing, fv::WaveformOverlay::kStateProcessing},
                 std::pair{OverlayState::Transcribing, fv::WaveformOverlay::kStateTranscribing},
                 std::pair{OverlayState::Cleaning, fv::WaveformOverlay::kStateCleaning},
                 std::pair{OverlayState::Rewriting, fv::WaveformOverlay::kStateRewriting});
    ControllerRig rig;
    rig.ui().overlayRouter().setOverlayState(state);

    rig.ui().showNotice(QStringLiteral("A transcription is already in progress — wait before "
                                       "starting another recording"),
                        false);

    CHECK(rig.overlay().currentState() == name);
    CHECK(rig.overlay().isVisible());
    CHECK_FALSE(rig.overlay().grab().isNull());
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_the_main_window_in_front_gets_no_notice
TEST_CASE("test_the_main_window_in_front_gets_no_notice",
          "[test_personalize_fix3_h1_outcome_notices][widget]") {
    ControllerRig rig;
    fv::OverlayRouter& router = rig.ui().overlayRouter();

    router.showNotice(QStringLiteral("Nothing to paste yet"), false, true);
    CHECK_FALSE(rig.overlay().isVisible());

    router.showNotice(QStringLiteral("Original pasted"), true, false);
    CHECK(rig.overlay().isVisible());
    CHECK(rig.overlay().currentState() == fv::WaveformOverlay::kStateNotice);
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_failed_paste_of_the_original_is_reported
TEST_CASE("test_a_failed_paste_of_the_original_is_reported",
          "[test_personalize_fix3_h1_outcome_notices][widget]") {
    ControllerRig rig;
    NoticeLog notices(rig);
    rig.keys().fail = true;

    CHECK_FALSE(rig.runtime().paste_text_now("um hello"));

    CHECK(notices.shown == std::vector<std::pair<std::string, bool>>{
                               {"Transcription complete (paste failed)", false}});
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_paste_and_copy_original_report_where_the_user_is
// (through the controller's History actions; the window is not in front, so the overlay shows
// each outcome)
TEST_CASE("test_paste_and_copy_original_report_where_the_user_is",
          "[test_personalize_fix3_h1_outcome_notices][widget]") {
    ControllerRig rig;
    REQUIRE_FALSE(rig.window().isActiveWindow());

    rig.ui().pasteLastOriginal();
    CHECK(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kNothingYet);
    }));
    CHECK(waitUntil(
        [&rig] { return rig.overlay().currentState() == fv::WaveformOverlay::kStateNotice; }));
    rig.ui().copyLastOriginal();
    CHECK(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kNothingToCopy);
    }));

    saveDictation(rig, "Hello, world.", std::string("um hello world"));
    rig.ui().pasteLastOriginal();
    CHECK(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kPasted);
    }));
    rig.ui().copyLastOriginal();
    CHECK(waitUntil([&rig] {
        return rig.window().status() == QString::fromUtf8(fv::history_actions::kCopied);
    }));
    CHECK(rig.keys().sent() == std::vector<std::string>{"paste"});
}
