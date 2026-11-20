// The finalization card's decision table and the tab's pure helpers
// (ui_qt/widgets/meeting_mode_tab.py). The golden replay covers the table case by case; these
// pin the rows the Meeting Mode tab depends on.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "meeting/finalization_view.h"
#include "pyport/errors.h"

using fv::Json;
using namespace fv::meeting;

namespace {

CardDecision card_for(const Json& payload, bool background = false) {
    return decide_card(FinalizationView::read(payload), background);
}

const Json kRunningSteps = Json::array({
    {{"id", "redecode"}, {"name", "Audio Re-transcription"}, {"status", "completed"}},
    {{"id", "speaker_id"}, {"name", "Speaker Identification"}, {"status", "running"}},
    {{"id", "finalize"}, {"name", "State Finalization"}, {"status", "pending"}},
});

} // namespace

TEST_CASE("a running card shows its step and progress", "[meeting][finalization_view]") {
    const CardDecision card = card_for({{"status", "running"},
                                        {"message", "Identifying speakers…"},
                                        {"current_step", 2},
                                        {"total_steps", 3},
                                        {"step_details", "Uploading"},
                                        {"steps", kRunningSteps}},
                                       true);
    CHECK(card.title == "Finalizing Meeting");
    CHECK(card.tone == "neutral");
    CHECK(card.step_badge == "Step 2 of 3");
    CHECK(card.progress_value == 50);
    CHECK(card.progress_maximum == 100);
    CHECK(card.show_detail);
    CHECK(card.detail == "Uploading");
    CHECK(card.show_background);
    CHECK_FALSE(card.show_retry);
    CHECK_FALSE(card.show_done);
    CHECK_FALSE(card.report_enabled);
    CHECK(card.report_tip == "The final report is still being prepared");
    REQUIRE(card.steps.size() == 3);
    CHECK(card.steps[1].running);
    CHECK(card.steps[1].icon == StepIcon{"●", "@accent"});
    // Running rows offer no actions.
    CHECK(card.steps[0].action_label.empty());
}

TEST_CASE("a running card without steps is indeterminate", "[meeting][finalization_view]") {
    const CardDecision card = card_for({{"status", "running"}});
    CHECK(card.step_badge == "In progress");
    CHECK(card.progress_minimum == 0);
    CHECK(card.progress_maximum == 0);
    CHECK_FALSE(card.show_steps);
    CHECK_FALSE(card.show_background);
}

TEST_CASE("a completed card with a failed step finishes with issues",
          "[meeting][finalization_view]") {
    const CardDecision card =
        card_for({{"status", "completed"},
                  {"steps", Json::array({{{"id", "redecode"}, {"status", "failed"}},
                                         {{"id", "finalize"}, {"status", "completed"}}})},
                  {"content_summary", {{"can_rerun_speakers", true}}}});
    CHECK(card.title == "Meeting Finished With Issues");
    CHECK(card.message == "Some post-meeting steps failed. The recording was kept.");
    CHECK(card.tone == "warning");
    CHECK(card.show_retry);
    CHECK(card.show_keep_later);
    CHECK(card.show_rerun_speakers);
    CHECK(card.rerun_speakers_enabled);
    REQUIRE(card.steps.size() == 2);
    CHECK(card.steps[0].action_label == "Retry");
    CHECK(card.steps[0].name == "Step 1");
    CHECK(card.steps[1].action_label.empty());
}

TEST_CASE("a clean completion offers Done and run-again rows", "[meeting][finalization_view]") {
    const CardDecision card =
        card_for({{"status", "completed"},
                  {"steps", Json::array({{{"id", "speaker_id"}, {"status", "completed"}},
                                         {{"id", "finalize"}, {"status", "completed"}}})},
                  {"content_summary",
                   {{"can_rerun_speakers", true}, {"speaker_rerun_reason", "No consent."}}}});
    CHECK(card.tone == "success");
    CHECK(card.show_done);
    CHECK_FALSE(card.show_active_box);
    // The speaker step is in the list, so the separate re-run is not offered.
    CHECK_FALSE(card.show_rerun_speakers);
    REQUIRE(card.steps.size() == 2);
    CHECK(card.steps[0].action_label == "Run again");
    CHECK_FALSE(card.steps[0].action_enabled);
    CHECK(card.steps[0].action_tooltip == "No consent.");
    CHECK(card.rerun_speakers_tip == "No consent.");
}

TEST_CASE("disabled and empty meetings", "[meeting][finalization_view]") {
    CardDecision card = card_for({{"status", "disabled"}});
    CHECK(card.title == "AI Insights Off");
    CHECK(card.message == "AI insights are off for this meeting.");
    CHECK(card.tone == "info");
    CHECK(card.show_done);
    CHECK_FALSE(card.show_steps);
    CHECK(card.show_rerun_speakers);
    CHECK_FALSE(card.rerun_speakers_enabled);
    CHECK(card.rerun_speakers_tip ==
          "No system-audio recording is available for speaker identification");

    card = card_for({{"status", "failed"},
                     {"content_summary", {{"is_empty", true}, {"meeting_status", "failed"}}}});
    CHECK(card.title == "Meeting Failed");
    CHECK(card.message == "No audio or transcript was captured. The meeting failed before it "
                          "could start, so no dashboard was created.");
    CHECK(card.keep_hint == "This failed start has no audio or transcript.");
    CHECK(card.report_tip == "No report for this meeting");

    card = card_for({{"status", "completed"}, {"content_summary", {{"is_empty", true}}}});
    CHECK(card.title == "Empty Meeting");
    CHECK(card.show_active_box);
}

TEST_CASE("reading a payload follows Python's conversions", "[meeting][finalization_view]") {
    const FinalizationView view = FinalizationView::read({{"status", "running"},
                                                          {"message", "  hi \n"},
                                                          {"current_step", 2.9},
                                                          {"total_steps", "4"},
                                                          {"steps", nullptr}});
    CHECK(view.message == "hi");
    CHECK(view.current_step == 2);
    CHECK(view.total_steps == 4);
    CHECK(view.steps == Json::array());
    CHECK_THROWS_AS(FinalizationView::read({{"current_step", "x"}}), fv::py::ValueError);
    CHECK_THROWS_AS(FinalizationView::read({{"content_summary", "x"}}), fv::py::TypeError);
}

TEST_CASE("progress and the elapsed clock", "[meeting][finalization_view]") {
    CHECK(progress_percent(1, 4) == 12);
    CHECK(progress_percent(0, 4) == 5);
    CHECK(progress_percent(4, 4) == 87);
    CHECK(progress_percent(5, 4) == 95);
    CHECK_THROWS_AS(progress_percent(1, 0), fv::py::PyError);
    CHECK(elapsed_text(0.0) == "00:00");
    CHECK(elapsed_text(61.9) == "01:01");
    CHECK(elapsed_text(3599.9) == "59:59");
    CHECK(elapsed_text(3600.0) == "1:00:00");
    CHECK(elapsed_text(36061.0) == "10:01:01");
    CHECK(elapsed_text(-4.0) == "00:00");
}

TEST_CASE("platform copy for Windows", "[meeting][finalization_view]") {
    const auto [subtitle, hint] = meeting_audio_support_copy({"win32", "AMD64", ""});
    CHECK(subtitle.starts_with("Capture microphone audio"));
    CHECK(hint == "System audio uses Windows WASAPI loopback when available.");
    CHECK(meeting_audio_platform_tone({"linux", "x86_64", ""}) == "note");
    CHECK(meeting_audio_platform_tone({"linux", "i686", ""}) == "warning");
    CHECK(meeting_audio_shows_platform_warning({"linux", "x86_64", ""}));
    CHECK_FALSE(meeting_audio_shows_platform_warning({"darwin", "arm64", "13.1"}));
}
