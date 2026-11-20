#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <format>
#include <memory>
#include <string>
#include <vector>

#include "command_support.h"
#include "dictation/command_selection.h"
#include "hotkeys/dispatcher.h"
#include "llm/rewrite.h"

// Command Mode and transform outcomes show near the pointer: they run from another app while the
// window sits in the tray, where its status line is out of sight
// (tests/test_personalize_fix3_h1_outcome_notices.py, the runtime's half; the overlay's drawing
// of a notice is tests/ui_overlay's).

using namespace fv::dictation;
using namespace fv::dictation::testing;
using Catch::Matchers::ContainsSubstring;
using fv::focus::FocusSnapshot;

namespace {

CommandRigOptions notice_options() {
    CommandRigOptions options;
    options.settings["auto_paste"] = false;
    return options;
}

std::string busy(std::string_view action) {
    return std::format("A transcription is already in progress — wait before {}", action);
}

} // namespace

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_refused_command_says_why_after_the_overlay_hides
TEST_CASE("test_a_refused_command_says_why_after_the_overlay_hides",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    rig.model->answer(std::string(fv::llm::kNeedsSelection));

    REQUIRE(rig.runtime->claim_job(command_job("")));
    rig.transcribe();

    const auto entries = rig.trace.entries();
    REQUIRE(entries.size() >= 2);
    CHECK(std::vector<std::string>(entries.end() - 2, entries.end()) ==
          std::vector<std::string>{"overlay:none", "notice:Select the text to change first"});
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_rewrite_for_an_app_that_lost_focus_says_it_was_copied
TEST_CASE("test_a_rewrite_for_an_app_that_lost_focus_says_it_was_copied",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    rig.model->answer("Rewritten.");
    rig.capture.fake(FocusSnapshot{}, browser_app());

    REQUIRE(rig.runtime->claim_job(command_job("old", FocusSnapshot{notepad_app(), {}})));
    rig.transcribe();

    CHECK(rig.notices() ==
          std::vector<std::string>{"Rewrite copied — the app changed; paste it where you want"});
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_pasted_rewrite_and_a_written_command_are_confirmed
TEST_CASE("test_a_pasted_rewrite_and_a_written_command_are_confirmed",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    rig.capture.fake(FocusSnapshot{}, notepad_app());
    rig.model->answer("Dear Sir or Madam,");
    REQUIRE(rig.runtime->claim_job(command_job("hey you", FocusSnapshot{notepad_app(), {}})));
    rig.transcribe();
    CHECK(rig.notices() == std::vector<std::string>{"Rewritten (done)"});

    rig.model->answer("Thanks for the notes.");
    REQUIRE(rig.runtime->claim_job(command_job("", FocusSnapshot{notepad_app(), {}})));
    rig.transcribe();
    REQUIRE_FALSE(rig.notices().empty());
    CHECK(rig.notices().back() == "Inserted (done)");
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_failed_paste_of_a_rewrite_is_reported
TEST_CASE("test_a_failed_paste_of_a_rewrite_is_reported",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    rig.capture.fake(FocusSnapshot{}, notepad_app());
    rig.model->answer("Formal.");
    rig.keys.fail_paste = true;
    REQUIRE(rig.runtime->claim_job(command_job("hey", FocusSnapshot{notepad_app(), {}})));
    rig.transcribe();

    REQUIRE(rig.notices().size() == 1);
    CHECK(rig.notices()[0] == rig.last_status());
    CHECK_THAT(rig.notices()[0], ContainsSubstring("paste failed"));
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_an_ai_failure_in_a_command_is_reported
TEST_CASE("test_an_ai_failure_in_a_command_is_reported",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    rig.model->stall();
    REQUIRE(rig.runtime->claim_job(command_job("hey", FocusSnapshot{notepad_app(), {}})));
    rig.transcribe();

    CHECK(rig.notices() ==
          std::vector<std::string>{std::format("Error: {}", fv::llm::kTimedOutMessage)});
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_transform_with_nothing_selected_says_so
TEST_CASE("test_a_transform_with_nothing_selected_says_so",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    rig.commands->run_transform("polish");

    REQUIRE(rig.settle([&] { return !rig.notices().empty(); }));
    CHECK(rig.notices() == std::vector<std::string>{"Select text to transform"});
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_transforms_and_command_mode_without_a_provider_say_so
TEST_CASE("test_transforms_and_command_mode_without_a_provider_say_so",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRigOptions options = notice_options();
    options.provider_key = false;
    CommandRig rig(options);

    // The transform's provider check reads keys off the UI thread, so its answer is awaited
    // before the next shortcut, keeping Python's order.
    rig.commands->run_transform("polish");
    REQUIRE(rig.settle([&] { return rig.notices().size() == 1; }));
    rig.commands->run_transform("no-such-transform");
    rig.commands->key_pressed(monotonic_now());

    CHECK(rig.notices() == std::vector<std::string>{"Set up AI cleanup to use transforms",
                                                    "That transform no longer exists",
                                                    std::string(NO_PROVIDER_MESSAGE)});
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_command_during_a_recording_says_to_finish_it
TEST_CASE("test_a_command_during_a_recording_says_to_finish_it",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    rig.recorder.recording = true;

    rig.commands->key_pressed(monotonic_now());

    CHECK(rig.notices() ==
          std::vector<std::string>{"Finish the current recording before using Command Mode"});
}

// from tests/test_personalize_fix3_h1_outcome_notices.py::test_a_canceled_rewrite_gets_no_notice
TEST_CASE("test_a_canceled_rewrite_gets_no_notice", "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    REQUIRE(rig.runtime->claim_job(command_job("hey", FocusSnapshot{notepad_app(), {}})));
    rig.runtime->job_slot().request_cancel();

    rig.runtime->on_transcription_error("Transcription canceled");
    rig.drain();

    CHECK(rig.notices().empty());
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_a_transform_canceled_while_its_selection_is_read_gets_no_notice
TEST_CASE("test_a_transform_canceled_while_its_selection_is_read_gets_no_notice",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    rig.capture.fake(FocusSnapshot{}, notepad_app());
    rig.keys.hold();

    rig.commands->run_transform("polish");
    REQUIRE(rig.settle([&] { return rig.runtime->has_active_job(); }));
    REQUIRE(rig.settle([&] { return rig.keys.waits().size() == 1; }));
    rig.runtime->cancel();
    rig.keys.open();
    REQUIRE(rig.settle([&] { return !rig.runtime->has_active_job(); }));

    CHECK(rig.notices().empty());
}

// from
// tests/test_personalize_fix3_h1_outcome_notices.py::test_busy_refusals_of_rewrites_and_pastes_show_by_the_pointer
TEST_CASE("test_busy_refusals_of_rewrites_and_pastes_show_by_the_pointer",
          "[test_personalize_fix3_h1_outcome_notices]") {
    CommandRig rig(notice_options());
    REQUIRE(rig.runtime->claim_job(std::make_shared<const DictationJob>()));

    DictationJob transform;
    transform.mode = JobMode::Transform;
    transform.source = "hotkey";
    CHECK_FALSE(rig.runtime->begin_rewrite_job(
        std::make_shared<const DictationJob>(std::move(transform)), "Transform · Polish"));
    CHECK_FALSE(rig.runtime->paste_text_now("um hello"));

    CHECK(rig.notices() == std::vector<std::string>{busy("rewriting text"), busy("pasting")});
}
