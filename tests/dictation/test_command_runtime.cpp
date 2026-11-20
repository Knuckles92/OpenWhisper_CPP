#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "command_support.h"
#include "dictation/command_runtime.h"
#include "dictation/command_selection.h"
#include "hotkeys/hold_machine.h"
#include "llm/rewrite.h"

// Command Mode's shortcut, where the selection comes from, turning the transcript into text, and
// transforms (tests/test_personalize_s3_command_runtime.py), and refusals that keep no audio
// (tests/test_personalize_fix_f2_command_refusals.py). The Python tests run CommandRuntime over
// a fake transcription runtime; these run it over the real one, so a start shows as the recorder
// starting, a claim as the slot taken, a submitted rewrite as the job queued on the jobs strand.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Message;
using Catch::Matchers::MessageMatches;
using Catch::Matchers::StartsWith;
using fv::Json;
using fv::focus::FocusSnapshot;
using fv::focus::TextContext;

namespace {

constexpr double kHoldS = fv::hotkeys::kRecordMinHoldMs / 1000.0;

bool selection_done(const std::shared_ptr<const DictationJob>& job) {
    return resolved(job->selection, std::chrono::milliseconds::zero()).has_value();
}

std::string selection_value(const std::shared_ptr<const DictationJob>& job) {
    const auto value = resolved(job->selection, std::chrono::milliseconds::zero());
    REQUIRE(value.has_value());
    REQUIRE(std::holds_alternative<std::string>(*value));
    return std::get<std::string>(*value);
}

// _stop_and_read: hold, release, and the selection the stop read.
std::string stop_and_read(CommandRig& rig) {
    rig.commands->key_pressed(1.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    rig.commands->key_released(2.0);
    REQUIRE(rig.settle([&] { return selection_done(job); }));
    return selection_value(job);
}

// The transform job that claimed the slot.
std::shared_ptr<const DictationJob> claimed(CommandRig& rig) {
    return rig.runtime->job_slot().active_job();
}

} // namespace

// ---- the shortcut ----

// from
// tests/test_personalize_s3_command_runtime.py::test_hold_records_an_instruction_and_release_stops_it
TEST_CASE("test_hold_records_an_instruction_and_release_stops_it",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    auto service = rig.capture.fake(FocusSnapshot{notepad_app(), selected("old words")});

    rig.commands->key_pressed(10.0);
    CHECK(rig.trace.count("recorder_start") == 1);
    const auto job = rig.recording_job();
    REQUIRE(job);
    CHECK(job->mode == JobMode::Command);
    CHECK(service->requests == std::vector<std::pair<bool, bool>>{{false, true}});
    CHECK_FALSE(selection_done(job));

    rig.commands->key_released(10.0 + kHoldS + 0.05);
    CHECK(rig.trace.count("recorder_stop") == 1);
    REQUIRE(rig.settle([&] { return selection_done(job); }));
    CHECK(selection_value(job) == "old words");
    // UI Automation knew the selection, so no keys were sent at all.
    CHECK(rig.clipboard.captures().empty());
    CHECK(rig.keys.waits().empty());
}

// from tests/test_personalize_s3_command_runtime.py::test_a_short_tap_cancels
TEST_CASE("test_a_short_tap_cancels", "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.commands->key_pressed(10.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    rig.commands->key_released(10.1);

    CHECK(rig.trace.count("recorder_cancel") == 1);
    // A canceled recording never sends a copy.
    CHECK_FALSE(selection_done(job));
    CHECK(rig.clipboard.captures().empty());
    CHECK(rig.keys.waits().empty());
    rig.commands->key_released(11.0);
    CHECK(rig.trace.count("recorder_cancel") == 1);
    CHECK(rig.trace.count("recorder_stop") == 0);
}

// from
// tests/test_personalize_s3_command_runtime.py::test_toggle_mode_presses_start_and_stop_and_ignores_releases
TEST_CASE("test_toggle_mode_presses_start_and_stop_and_ignores_releases",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.settings.update(Json{{"recording_trigger_mode", "toggle"}});

    rig.commands->key_pressed(1.0);
    rig.commands->key_released(1.05);
    CHECK(rig.trace.count("recorder_stop") == 0);
    CHECK(rig.trace.count("recorder_cancel") == 0);
    rig.commands->key_pressed(5.0);
    rig.commands->key_released(5.1);

    CHECK(rig.trace.count("recorder_start") == 1);
    CHECK(rig.trace.count("recorder_stop") == 1);
    CHECK(rig.trace.count("recorder_cancel") == 0);
}

// from
// tests/test_personalize_s3_command_runtime.py::test_without_a_provider_it_says_so_and_never_records
TEST_CASE("test_without_a_provider_it_says_so_and_never_records",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig(CommandRigOptions{.provider_key = false});

    rig.commands->key_pressed(1.0);
    rig.commands->key_released(2.0);

    CHECK(rig.statuses() == std::vector<std::string>{"Set up AI cleanup to use Command Mode"});
    CHECK(rig.trace.count("recorder_start") == 0);
    CHECK(rig.trace.count("recorder_stop") == 0);
}

// from tests/test_personalize_s3_command_runtime.py::test_a_dictation_in_progress_is_left_alone
TEST_CASE("test_a_dictation_in_progress_is_left_alone", "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.recorder.recording = true;

    rig.commands->key_pressed(1.0);
    rig.commands->key_released(2.0);

    CHECK(rig.statuses() ==
          std::vector<std::string>{"Finish the current recording before using Command Mode"});
    CHECK(rig.trace.count("recorder_start") == 0);
    CHECK(rig.trace.count("recorder_stop") == 0);
    CHECK(rig.trace.count("recorder_cancel") == 0);
}

// from
// tests/test_personalize_s3_command_runtime.py::test_a_press_during_a_stops_post_roll_is_left_to_the_runtime
TEST_CASE("test_a_press_during_a_stops_post_roll_is_left_to_the_runtime",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.recorder.recording = true;
    REQUIRE(rig.runtime->claim_job());

    rig.commands->key_pressed(1.0);
    rig.commands->key_released(2.0);

    // The start is asked for and the runtime refuses it as busy, in its own words.
    CHECK(rig.statuses() == std::vector<std::string>{"A transcription is already in progress — "
                                                     "wait before starting another recording"});
    CHECK(rig.trace.count("recorder_start") == 0);
    CHECK(rig.trace.count("recorder_stop") == 0);
    CHECK(rig.trace.count("recorder_cancel") == 0);
}

// from tests/test_personalize_s3_command_runtime.py::test_a_refused_start_leaves_nothing_to_stop
TEST_CASE("test_a_refused_start_leaves_nothing_to_stop", "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.recorder.start_ok = false;

    rig.commands->key_pressed(1.0);
    rig.commands->key_released(2.0);

    CHECK(rig.trace.count("recorder_start") == 1);
    CHECK(rig.trace.count("recorder_stop") == 0);
    CHECK(rig.trace.count("recorder_cancel") == 0);
}

// from
// tests/test_personalize_s3_command_runtime.py::test_a_recording_canceled_elsewhere_lets_the_next_press_start_again
TEST_CASE("test_a_recording_canceled_elsewhere_lets_the_next_press_start_again",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.settings.update(Json{{"recording_trigger_mode", "toggle"}});
    rig.commands->key_pressed(1.0);
    const auto first = rig.recording_job();
    REQUIRE(first);
    REQUIRE(rig.runtime->cancel()); // the Cancel hotkey

    rig.commands->key_pressed(2.0);

    CHECK_FALSE(selection_done(first));
    CHECK(rig.trace.count("recorder_start") == 2);
    CHECK(rig.trace.count("recorder_stop") == 0);
    const auto second = rig.recording_job();
    REQUIRE(second);
    CHECK(second->id != first->id);
}

// from
// tests/test_personalize_s3_command_runtime.py::test_a_recording_stopped_elsewhere_still_reads_its_selection_once
TEST_CASE("test_a_recording_stopped_elsewhere_still_reads_its_selection_once",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.capture.fake(FocusSnapshot{notepad_app(), std::nullopt});
    rig.clipboard.set_selection("picked words");
    rig.commands->key_pressed(1.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    rig.runtime->stop_recording(); // the Stop button, not the shortcut

    const RewriteResult result =
        rig.on_worker([&] { return rig.commands->complete_recording("shorter", job); });

    CHECK(result.raw_text == std::optional<std::string>("picked words"));
    rig.commands->key_released(3.0);
    CHECK(rig.trace.count("recorder_stop") == 1);
    CHECK(rig.clipboard.captures() == std::vector<bool>{true});
}

// The shutdown order command_runtime.h asks for: a press on the dispatcher waiting for the UI
// thread to start its recording returns once the runtime is cleaned up, so the UI thread can stop
// the dispatcher next, and the start it queued does nothing when the UI thread gets to it.
TEST_CASE("a press waiting for the UI thread returns once the runtime is cleaned up",
          "[command_runtime]") {
    CommandRig rig;
    auto press = std::async(std::launch::async, [&rig] { rig.commands->key_pressed(1.0); });
    // The GUI thread is not pumped, so the press waits for its start.
    CHECK(press.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);

    rig.commands->cleanup();

    REQUIRE(press.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    press.get();
    rig.gui.pump();
    CHECK(rig.trace.count("recorder_start") == 0);
    CHECK_FALSE(rig.recording_job());
}

// ---- where the selection comes from ----

// from
// tests/test_personalize_s3_command_runtime.py::test_unknown_selection_is_copied_once_the_keys_are_up
TEST_CASE("test_unknown_selection_is_copied_once_the_keys_are_up",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    TextContext caret;
    caret.before = "x";
    caret.caret_known = true;
    rig.capture.fake(FocusSnapshot{notepad_app(), caret});
    rig.clipboard.set_selection("copied words");

    CHECK(stop_and_read(rig) == "copied words");
    // The modifier wait runs on the reader, the copy itself on the GUI thread, after it.
    CHECK(rig.keys.waits() == std::vector<bool>{false});
    CHECK(rig.clipboard.captures() == std::vector<bool>{true});
    // The worker finds the selection read and never copies again.
    const RewriteResult result =
        rig.commands->complete_recording("shorter", rig.runtime->current_job());
    CHECK(result.raw_text == std::optional<std::string>("copied words"));
    CHECK(rig.clipboard.captures() == std::vector<bool>{true});
}

// from
// tests/test_personalize_s3_command_runtime.py::test_an_empty_selection_from_ui_automation_is_final
TEST_CASE("test_an_empty_selection_from_ui_automation_is_final",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.capture.fake(FocusSnapshot{notepad_app(), selected("")});
    rig.clipboard.set_selection("should never be read");

    CHECK(stop_and_read(rig).empty());
    CHECK(rig.clipboard.captures().empty());
}

// from tests/test_personalize_s3_command_runtime.py::test_terminals_never_get_a_copy
TEST_CASE("test_terminals_never_get_a_copy", "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.capture.fake(FocusSnapshot{terminal_app(), std::nullopt});
    rig.clipboard.set_selection("would have interrupted");

    CHECK(stop_and_read(rig).empty());
    CHECK(rig.clipboard.captures().empty());
    CHECK(rig.keys.waits().empty());
}

// from
// tests/test_personalize_s3_command_runtime.py::test_copy_line_editors_only_count_real_selections
TEST_CASE("test_copy_line_editors_only_count_real_selections",
          "[test_personalize_s3_command_runtime]") {
    const auto [copied, expected] = GENERATE(table<std::string, std::string>({
        {"def main():\r\n", ""},
        {"\n", ""},
        {"main", "main"},
        {"line one\nline two\n", "line one\nline two\n"},
    }));
    CommandRig rig;
    rig.capture.fake(FocusSnapshot{vs_code_app(), std::nullopt});
    rig.clipboard.set_selection(copied);

    CHECK(stop_and_read(rig) == expected);
}

// from
// tests/test_personalize_s3_command_runtime.py::test_with_app_context_off_the_selection_is_still_read
TEST_CASE("test_with_app_context_off_the_selection_is_still_read",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.settings.update(Json{{"app_context_enabled", false}});
    auto service = rig.capture.fake(FocusSnapshot{notepad_app(), selected("mine")});

    rig.commands->key_pressed(1.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    // Asked for before the recording started, and never handed to the job; the job only learns
    // where to paste.
    CHECK(service->requests == std::vector<std::pair<bool, bool>>{{false, true}, {false, false}});
    CHECK(job->focus == nullptr);
    CHECK(job->target != nullptr);
    rig.commands->key_released(2.0);
    REQUIRE(rig.settle([&] { return selection_done(job); }));
    CHECK(selection_value(job) == "mine");
}

// ---- turning the transcript into text ----

// from
// tests/test_personalize_s3_command_runtime.py::test_a_selection_is_rewritten_by_the_instruction
TEST_CASE("test_a_selection_is_rewritten_by_the_instruction",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.model->answer("New words.");

    const RewriteResult result =
        rig.commands->complete_recording("  make it formal ", command_job("old words"));

    CHECK(result.text == "New words.");
    CHECK(result.raw_text == std::optional<std::string>("old words"));
    REQUIRE(result.info.has_value());
    CHECK(result.info->provider == "openrouter");
    CHECK(result.info->model == "test/model");
    CHECK(result.info->level.empty());
    CHECK(result.info->elapsed_s >= 0.0);
    const auto calls = rig.model->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].first == "old words");
    CHECK_THAT(calls[0].second, ContainsSubstring("Instruction:\nmake it formal"));
    CHECK(rig.overlays() == std::vector<std::string>{"rewriting"});
    CHECK(rig.statuses() == std::vector<std::string>{"Rewriting..."});
}

// from tests/test_personalize_s3_command_runtime.py::test_with_nothing_selected_it_writes_new_text
TEST_CASE("test_with_nothing_selected_it_writes_new_text",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.model->answer("Dear team,");

    const RewriteResult result =
        rig.commands->complete_recording("write a greeting", command_job(""));

    CHECK(result.text == "Dear team,");
    CHECK_FALSE(result.raw_text.has_value());
    REQUIRE(result.info.has_value());
    CHECK(result.info->provider == "openrouter");
    CHECK(result.info->model == "test/model");
    CHECK(result.info->level.empty());
    CHECK(result.info->elapsed_s < 1.0);
    const auto calls = rig.model->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].first == "write a greeting");
    CHECK(rig.statuses() == std::vector<std::string>{"Writing..."});
}

// from tests/test_personalize_s3_command_runtime.py::test_writing_new_text_can_be_turned_off
TEST_CASE("test_writing_new_text_can_be_turned_off", "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.settings.update(Json{{"command_mode_insert_without_selection", false}});

    CHECK_THROWS_MATCHES(rig.commands->complete_recording("write a greeting", command_job("")),
                         CommandRefused, Message("Select text first"));
    CHECK(rig.model->requests().empty());
}

// from tests/test_personalize_s3_command_runtime.py::test_silence_is_not_an_instruction
TEST_CASE("test_silence_is_not_an_instruction", "[test_personalize_s3_command_runtime]") {
    const std::string raw = GENERATE(std::string(), std::string("   "));
    CommandRig rig;

    CHECK_THROWS_MATCHES(rig.commands->complete_recording(raw, command_job("old words")),
                         CommandRefused,
                         MessageMatches(ContainsSubstring("Didn't catch an instruction")));
}

// from tests/test_personalize_s3_command_runtime.py::test_an_unavailable_provider_is_reported
TEST_CASE("test_an_unavailable_provider_is_reported", "[test_personalize_s3_command_runtime]") {
    CommandRig rig(CommandRigOptions{.cleanup_key = false});

    CHECK_THROWS_MATCHES(
        rig.commands->complete_recording("shorter", command_job("old words")), CommandRefused,
        MessageMatches(ContainsSubstring("Set up AI cleanup to use Command Mode")));
}

// from tests/test_personalize_s3_command_runtime.py::test_failed_rewrites_raise_instead_of_pasting
TEST_CASE("test_failed_rewrites_raise_instead_of_pasting",
          "[test_personalize_s3_command_runtime]") {
    const bool timeout = GENERATE(true, false);
    CommandRig rig;
    std::string selection = "old";
    std::string message = "took too long";
    if (timeout) {
        rig.model->stall();
    } else {
        rig.model->answer(std::string(fv::llm::kNeedsSelection));
        selection = "";
        message = "Select the text to change first";
    }

    CHECK_THROWS_MATCHES(
        rig.commands->complete_recording("make this shorter", command_job(selection)),
        std::runtime_error, MessageMatches(ContainsSubstring(message)));
}

// from
// tests/test_personalize_s3_command_runtime.py::test_a_selection_that_never_arrives_is_an_error
TEST_CASE("test_a_selection_that_never_arrives_is_an_error",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.commands->set_selection_wait(std::chrono::milliseconds(10));
    rig.capture.fake(FocusSnapshot{notepad_app(), std::nullopt});
    rig.clipboard.never_answer();
    rig.commands->key_pressed(1.0);
    const auto job = rig.recording_job();
    REQUIRE(job);

    CHECK_THROWS_MATCHES(
        rig.on_worker([&] { return rig.commands->complete_recording("shorter", job); }),
        CommandRefused, MessageMatches(StartsWith("Couldn't read the selected text")));
    CHECK(rig.model->requests().empty());
}

// from
// tests/test_personalize_s3_command_runtime.py::test_the_dictionary_corrects_the_spoken_instruction
TEST_CASE("test_the_dictionary_corrects_the_spoken_instruction",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.settings.update(Json{
        {"dictation_dictionary",
         Json::array({Json{{"id", "a"}, {"term", "Acme"}, {"heard", Json::array({"ackme"})}}})}});

    // A saved "sounds like" in place of Python's patched apply_replacements.
    rig.commands->complete_recording("mention ackme", command_job(""));

    const auto calls = rig.model->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].first == "mention Acme");
}

// ---- transforms ----

// from tests/test_personalize_s3_command_runtime.py::test_a_transform_rewrites_the_selection
TEST_CASE("test_a_transform_rewrites_the_selection", "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.capture.fake(FocusSnapshot{notepad_app(), selected("rough draft")});
    rig.model->answer("Polished draft.");

    rig.commands->run_transform("polish");

    REQUIRE(rig.settle([&] { return rig.runtime->has_active_job(); }));
    const auto job = claimed(rig);
    REQUIRE(job);
    CHECK(job->mode == JobMode::Transform);
    CHECK(rig.runtime->pending().source_name == std::optional<std::string>("Transform · Polish"));
    // Only its shortcut starts a transform.
    CHECK(job->source == "hotkey");
    REQUIRE(job->snapshot().has_value());
    CHECK(job->snapshot()->identity == notepad_app());
    CHECK(rig.statuses() == std::vector<std::string>{"Rewriting...", "Rewriting · Polish..."});
    REQUIRE(rig.settle([&] { return rig.jobs.queued() > 0; }));
    CHECK(selection_value(job) == "rough draft");
    rig.drain();
    const auto completed = rig.completed();
    REQUIRE(completed.size() == 1);
    CHECK(completed[0].text == "Polished draft.");
    CHECK(completed[0].raw == std::optional<std::string>("rough draft"));
    REQUIRE(completed[0].info.has_value());
    CHECK(completed[0].info->level.empty());
    const auto calls = rig.model->calls();
    REQUIRE(calls.size() == 1);
    CHECK_THAT(calls[0].second, ContainsSubstring("Improve the flow and clarity"));
}

// from tests/test_personalize_s3_command_runtime.py::test_an_empty_selection_abandons_the_transform
TEST_CASE("test_an_empty_selection_abandons_the_transform",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.capture.fake(FocusSnapshot{notepad_app(), std::nullopt});
    rig.clipboard.set_selection("  ");

    rig.commands->run_transform("polish");

    REQUIRE(rig.settle([&] { return rig.last_status() == "Select text to transform"; }));
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.keys.waits() == std::vector<bool>{false});
    CHECK(rig.clipboard.captures() == std::vector<bool>{true});
}

// from tests/test_personalize_s3_command_runtime.py::test_a_refused_claim_reads_nothing
TEST_CASE("test_a_refused_claim_reads_nothing", "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.clipboard.set_selection("text");
    REQUIRE(rig.runtime->claim_job());

    rig.commands->run_transform("polish");

    // The runtime refuses the claim as busy, in its own words, and nothing is read.
    REQUIRE(rig.settle([&] { return !rig.notices().empty(); }));
    rig.settle([] { return false; }, std::chrono::milliseconds(50));
    CHECK(rig.notices() ==
          std::vector<std::string>{"A transcription is already in progress — wait before rewriting "
                                   "text"});
    CHECK(rig.clipboard.captures().empty());
    CHECK(rig.keys.waits().empty());
    CHECK(rig.trace.with_prefix("status:Rewriting").empty());
}

// from
// tests/test_personalize_s3_command_runtime.py::test_unknown_transforms_and_missing_providers_say_so
TEST_CASE("test_unknown_transforms_and_missing_providers_say_so",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig(CommandRigOptions{.provider_key = false});

    rig.commands->run_transform("missing");
    rig.commands->run_transform("polish");

    REQUIRE(rig.settle([&] { return rig.statuses().size() == 2; }));
    CHECK(rig.statuses() == std::vector<std::string>{"That transform no longer exists",
                                                     "Set up AI cleanup to use transforms"});
    CHECK_FALSE(rig.runtime->has_active_job());
}

// from
// tests/test_personalize_s3_command_runtime.py::test_transforms_keep_the_app_out_of_the_job_when_app_context_is_off
TEST_CASE("test_transforms_keep_the_app_out_of_the_job_when_app_context_is_off",
          "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.settings.update(Json{{"app_context_enabled", false}});
    rig.capture.fake(FocusSnapshot{notepad_app(), selected("text")});

    rig.commands->run_transform("fix-grammar");

    REQUIRE(rig.settle([&] { return rig.runtime->has_active_job(); }));
    const auto job = claimed(rig);
    REQUIRE(job);
    CHECK(job->focus == nullptr);
    REQUIRE(rig.settle([&] { return rig.jobs.queued() > 0; }));
    CHECK(selection_value(job) == "text");
}

// from tests/test_personalize_s3_command_runtime.py::test_a_closed_runtime_ignores_shortcuts
TEST_CASE("test_a_closed_runtime_ignores_shortcuts", "[test_personalize_s3_command_runtime]") {
    CommandRig rig;
    rig.commands->cleanup();
    rig.commands->key_pressed(1.0);
    rig.commands->run_transform("polish");
    rig.settle([] { return false; }, std::chrono::milliseconds(50));

    CHECK(rig.trace.count("recorder_start") == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.statuses().empty());
}

// ---- tests/test_personalize_fix_f2_command_refusals.py ----

namespace {

// The refusals fixture: auto-paste on, the engine hears "make this shorter", and a stopped
// Command Mode recording goes from its saved WAV to delivery.
struct RefusalRig {
    explicit RefusalRig(std::string heard = "make this shorter")
        : rig([&] {
              CommandRigOptions options;
              options.heard = std::move(heard);
              return options;
          }()) {}

    void record_command(std::string selection = "") {
        REQUIRE(rig.runtime->claim_job(command_job(std::move(selection))));
        rig.on_worker([this] { rig.runtime->finish_recording_job(); });
        rig.drain();
    }
    void check_refused(const std::string& message) {
        CHECK(rig.last_status() == message);
        CHECK(rig.recordings() == 0);
        CHECK(rig.recorder.acknowledged == 1);
        CHECK(rig.trace.count("paste") == 0);
        CHECK(rig.entries().empty());
        CHECK_FALSE(rig.runtime->pending().audio_path.has_value());
        CHECK_FALSE(rig.runtime->has_active_job());
    }

    CommandRig rig;
};

} // namespace

// from
// tests/test_personalize_fix_f2_command_refusals.py::test_an_instruction_that_needs_a_selection_keeps_no_audio
TEST_CASE("test_an_instruction_that_needs_a_selection_keeps_no_audio",
          "[test_personalize_fix_f2_command_refusals]") {
    RefusalRig refusals;
    refusals.rig.model->answer(std::string(fv::llm::kNeedsSelection));

    refusals.record_command();

    refusals.check_refused("Select the text to change first");
}

// from
// tests/test_personalize_fix_f2_command_refusals.py::test_an_instruction_nobody_heard_keeps_no_audio
TEST_CASE("test_an_instruction_nobody_heard_keeps_no_audio",
          "[test_personalize_fix_f2_command_refusals]") {
    RefusalRig refusals("");

    refusals.record_command();

    refusals.check_refused("Didn't catch an instruction");
}

// from
// tests/test_personalize_fix_f2_command_refusals.py::test_nothing_selected_with_writing_turned_off_keeps_no_audio
TEST_CASE("test_nothing_selected_with_writing_turned_off_keeps_no_audio",
          "[test_personalize_fix_f2_command_refusals]") {
    RefusalRig refusals;
    refusals.rig.settings.update(Json{{"command_mode_insert_without_selection", false}});

    refusals.record_command();

    refusals.check_refused("Select text first");
}

// from
// tests/test_personalize_fix_f2_command_refusals.py::test_a_failing_ai_model_still_keeps_the_instruction_audio
TEST_CASE("test_a_failing_ai_model_still_keeps_the_instruction_audio",
          "[test_personalize_fix_f2_command_refusals]") {
    RefusalRig refusals;
    refusals.rig.model->stall();

    refusals.record_command("old words");

    CHECK(refusals.rig.recordings() == 1);
    CHECK_THAT(refusals.rig.last_status(),
               StartsWith(std::format("Error: {} — audio saved in Recordings as ",
                                      fv::llm::kTimedOutMessage)));
    CHECK_FALSE(refusals.rig.runtime->has_active_job());
}

// from
// tests/test_personalize_fix_f2_command_refusals.py::test_a_transform_refusal_shows_its_message_plainly
TEST_CASE("test_a_transform_refusal_shows_its_message_plainly",
          "[test_personalize_fix_f2_command_refusals]") {
    RefusalRig refusals;
    auto& rig = refusals.rig;
    REQUIRE(rig.runtime->begin_rewrite_job(
        command_job(std::nullopt, std::nullopt, JobMode::Transform), "Transform · Polish"));
    const std::string too_long(fv::llm::kMaxTextChars + 1, 'x');

    rig.runtime->submit_rewrite(
        [&rig, too_long] { return rig.commands->rewrite_text(too_long, "Polish it"); });
    rig.drain();

    CHECK_THAT(rig.last_status(), StartsWith("Select less text to rewrite"));
    CHECK(rig.recordings() == 0);
    CHECK(rig.trace.count("paste") == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
}
