#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "command_support.h"
#include "focus/catalog.h"
#include "llm/rewrite.h"

// Command Mode and transforms through the real transcription runtime: paste and History
// (tests/test_personalize_s3_command_delivery.py), the paste check without app awareness
// (tests/test_personalize_fix_f2_paste_target.py) and across a browser's tabs
// (tests/test_personalize_fix2_g1_paste_target_tab.py).

using namespace fv::dictation;
using namespace fv::dictation::testing;
using Catch::Matchers::StartsWith;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::FocusSnapshot;

namespace {

constexpr std::string_view CHANGED_STATUS =
    "Rewrite copied — the app changed; paste it where you want";
constexpr std::string_view UNKNOWN_STATUS =
    "Rewrite copied — couldn't confirm the app; paste it where you want";

// h: auto-paste off (a rewrite pastes anyway), the engine hears "make it formal".
CommandRigOptions delivery_options() {
    CommandRigOptions options;
    options.settings["auto_paste"] = false;
    return options;
}

// tests/test_personalize_fix_f2_paste_target.py h: app awareness off as well.
CommandRigOptions unaware_options() {
    CommandRigOptions options = delivery_options();
    options.settings["app_context_enabled"] = false;
    return options;
}

// Runs a transform from its shortcut to its History save.
void run_transform(CommandRig& rig, const std::string& id) {
    rig.commands->run_transform(id);
    REQUIRE(rig.settle([&] { return rig.jobs.queued() > 0 || !rig.runtime->has_active_job(); }));
    REQUIRE(rig.settle([&] { return rig.jobs.queued() > 0; }));
    rig.drain();
}

AppIdentity chrome(std::string_view title) {
    AppIdentity identity = app("chrome.exe", fv::focus::catalog::app_name("chrome.exe"), 4242,
                               "0x1a2b", fv::focus::catalog::title_hint("chrome.exe", title));
    identity.platform = "windows";
    return identity;
}

} // namespace

// ---- the package's acceptance: a scripted Command Mode run ----

// Hold the shortcut, speak, release: the selection is read once through the clipboard after the
// modifiers come up, the model's answer is pasted (auto-paste off) into the app the shortcut was
// pressed in with "Rewritten" by the pointer, and History keeps it as "Command Mode" with the
// selection as its raw text. With the app changed meanwhile, the answer is copied instead.
TEST_CASE("a scripted Command Mode run rewrites the selection in the same app",
          "[test_personalize_s3_command_delivery]") {
    const bool app_changed = GENERATE(false, true);
    CAPTURE(app_changed);
    CommandRig rig(delivery_options());
    rig.capture.fake(FocusSnapshot{notepad_app(), std::nullopt},
                     app_changed ? browser_app() : notepad_app());
    rig.clipboard.set_selection("hey you");
    rig.model->answer("Dear Sir or Madam,");

    rig.commands->key_pressed(10.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    CHECK(job->mode == JobMode::Command);
    CHECK(rig.last_status() == "Command Mode · Listening...");
    REQUIRE_FALSE(rig.overlays().empty());
    CHECK(rig.overlays().back() == "command_listening");

    rig.commands->key_released(12.0);
    rig.drain();

    // Read once, after the modifier wait.
    CHECK(rig.keys.waits() == std::vector<bool>{false});
    CHECK(rig.clipboard.captures() == std::vector<bool>{true});
    CHECK(rig.trace.count("send_copy") == 1);
    const auto statuses = rig.statuses();
    CHECK(std::ranges::find(statuses, "Rewriting...") != statuses.end());
    const auto calls = rig.model->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].first == "hey you");
    if (app_changed) {
        CHECK(rig.pastes() == 0);
        CHECK(rig.copies() == std::vector<std::string>{"Dear Sir or Madam,"});
        CHECK(rig.notices() == std::vector<std::string>{std::string(CHANGED_STATUS)});
    } else {
        CHECK(rig.stages() == std::vector<std::string>{"Dear Sir or Madam,"});
        CHECK(rig.pastes() == 1);
        CHECK(rig.notices() == std::vector<std::string>{"Rewritten (done)"});
    }
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Dear Sir or Madam,");
    CHECK(entries[0].raw_text == std::optional<std::string>("hey you"));
    CHECK(entries[0].source_name == std::optional<std::string>("Command Mode"));
    CHECK_FALSE(rig.runtime->has_active_job());
}

// ---- tests/test_personalize_s3_command_delivery.py ----

// from
// tests/test_personalize_s3_command_delivery.py::test_a_command_pastes_its_rewrite_and_keeps_the_selection_as_raw_text
TEST_CASE("test_a_command_pastes_its_rewrite_and_keeps_the_selection_as_raw_text",
          "[test_personalize_s3_command_delivery]") {
    CommandRig rig(delivery_options());
    rig.model->answer("Dear Sir or Madam,");
    rig.capture.fake(FocusSnapshot{}, notepad_app());

    REQUIRE(rig.runtime->claim_job(command_job("hey you", FocusSnapshot{notepad_app(), {}})));
    rig.runtime->set_pending(PendingAudio{.source_name = "Command Mode"});
    rig.transcribe();

    CHECK(rig.stages() == std::vector<std::string>{"Dear Sir or Madam,"});
    CHECK(rig.pastes() == 1);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Dear Sir or Madam,");
    CHECK(entries[0].raw_text == std::optional<std::string>("hey you"));
    CHECK(entries[0].entry_kind == std::optional<std::string>("command"));
    CHECK(entries[0].source_name == std::optional<std::string>("Command Mode"));
    CHECK(entries[0].cleanup_provider == std::optional<std::string>("openrouter"));
    CHECK_FALSE(rig.runtime->has_active_job());
}

// from
// tests/test_personalize_s3_command_delivery.py::test_a_command_never_pastes_into_an_app_that_took_focus
TEST_CASE("test_a_command_never_pastes_into_an_app_that_took_focus",
          "[test_personalize_s3_command_delivery]") {
    CommandRig rig(delivery_options());
    rig.model->answer("Rewritten.");
    rig.capture.fake(FocusSnapshot{}, browser_app());

    REQUIRE(rig.runtime->claim_job(command_job("old", FocusSnapshot{notepad_app(), {}})));
    rig.transcribe();

    CHECK(rig.pastes() == 0);
    CHECK(rig.copies() == std::vector<std::string>{"Rewritten."});
    CHECK_THAT(rig.last_status(), StartsWith("Rewrite copied"));
}

// from tests/test_personalize_s3_command_delivery.py::test_a_failed_command_pastes_nothing
TEST_CASE("test_a_failed_command_pastes_nothing", "[test_personalize_s3_command_delivery]") {
    CommandRig rig(delivery_options());
    rig.model->answer(std::string(fv::llm::kNeedsSelection));

    REQUIRE(rig.runtime->claim_job(command_job("")));
    rig.transcribe();

    CHECK(rig.pastes() == 0);
    CHECK(rig.entries().empty());
    CHECK(rig.last_status() == "Select the text to change first");
    CHECK_FALSE(rig.runtime->has_active_job());
}

// from
// tests/test_personalize_s3_command_delivery.py::test_a_transform_runs_through_the_job_slot_to_paste_and_history
TEST_CASE("test_a_transform_runs_through_the_job_slot_to_paste_and_history",
          "[test_personalize_s3_command_delivery]") {
    CommandRig rig(delivery_options());
    rig.capture.fake(FocusSnapshot{notepad_app(), selected("teh draft")}, notepad_app());
    rig.model->answer("The draft.");

    rig.commands->run_transform("fix-grammar");
    REQUIRE(rig.settle([&] { return rig.runtime->has_active_job(); }));
    REQUIRE(rig.settle([&] { return rig.jobs.queued() > 0; }));
    rig.drain();

    CHECK(rig.pastes() == 1);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "The draft.");
    CHECK(entries[0].raw_text == std::optional<std::string>("teh draft"));
    CHECK(entries[0].entry_kind == std::optional<std::string>("transform"));
    CHECK(entries[0].source_name == std::optional<std::string>("Transform · Fix grammar"));
    CHECK(entries[0].app_id == std::optional<std::string>("notepad.exe"));
    CHECK_FALSE(rig.runtime->has_active_job());
}

// from
// tests/test_personalize_s3_command_delivery.py::test_a_transform_with_nothing_selected_frees_the_slot
TEST_CASE("test_a_transform_with_nothing_selected_frees_the_slot",
          "[test_personalize_s3_command_delivery]") {
    CommandRig rig(delivery_options());

    rig.commands->run_transform("polish");
    REQUIRE(rig.settle([&] { return rig.last_status() == "Select text to transform"; }));

    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.pastes() == 0);
    CHECK(rig.entries().empty());
}

// from
// tests/test_personalize_s3_command_delivery.py::test_command_recordings_are_named_command_mode_in_history
TEST_CASE("test_command_recordings_are_named_command_mode_in_history",
          "[test_personalize_s3_command_delivery]") {
    CommandRig rig(delivery_options());
    rig.model->answer("Formal.");

    REQUIRE(rig.runtime->claim_job(command_job("hey")));
    rig.on_worker([&] { rig.runtime->finish_recording_job(); });
    rig.drain();

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source_name == std::optional<std::string>("Command Mode"));
    CHECK(entries[0].text == "Formal.");
}

// from
// tests/test_personalize_s3_command_delivery.py::test_a_transform_run_through_the_runtime_is_named_once
TEST_CASE("test_a_transform_run_through_the_runtime_is_named_once",
          "[test_personalize_s3_command_delivery]") {
    // The runtime's half: the entry names the transform once, as the History card's chip
    // (ui_history's kind_label) shows it.
    CommandRig rig(delivery_options());
    rig.capture.fake(FocusSnapshot{notepad_app(), selected("teh draft")}, notepad_app());
    rig.model->answer("The draft.");

    run_transform(rig, "fix-grammar");

    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry_kind == std::optional<std::string>("transform"));
    CHECK(entries[0].source_name == std::optional<std::string>("Transform · Fix grammar"));
}

// ---- tests/test_personalize_fix_f2_paste_target.py ----

// from
// tests/test_personalize_fix_f2_paste_target.py::test_without_app_awareness_a_transform_is_copied_when_the_app_changed
TEST_CASE("test_without_app_awareness_a_transform_is_copied_when_the_app_changed",
          "[test_personalize_fix_f2_paste_target]") {
    CommandRig rig(unaware_options());
    rig.model->answer("CONFIDENTIAL rewrite");
    rig.capture.fake(FocusSnapshot{notepad_app(), selected("CONFIDENTIAL draft")}, browser_app());

    run_transform(rig, "polish");

    CHECK(rig.pastes() == 0);
    CHECK(rig.copies() == std::vector<std::string>{"CONFIDENTIAL rewrite"});
    CHECK(rig.last_status() == CHANGED_STATUS);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK_FALSE(entries[0].app_id.has_value());
    CHECK_FALSE(entries[0].app_name.has_value());
}

// from
// tests/test_personalize_fix_f2_paste_target.py::test_without_app_awareness_a_transform_still_pastes_into_the_same_app
TEST_CASE("test_without_app_awareness_a_transform_still_pastes_into_the_same_app",
          "[test_personalize_fix_f2_paste_target]") {
    CommandRig rig(unaware_options());
    rig.model->answer("CONFIDENTIAL rewrite");
    rig.capture.fake(FocusSnapshot{notepad_app(), selected("CONFIDENTIAL draft")}, notepad_app());

    run_transform(rig, "polish");

    CHECK(rig.pastes() == 1);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK_FALSE(entries[0].app_id.has_value());
}

// from
// tests/test_personalize_fix_f2_paste_target.py::test_without_app_awareness_command_mode_checks_the_app_too
TEST_CASE("test_without_app_awareness_command_mode_checks_the_app_too",
          "[test_personalize_fix_f2_paste_target]") {
    const bool same_app = GENERATE(false, true);
    CommandRig rig(unaware_options());
    rig.capture.fake(FocusSnapshot{notepad_app(), selected("CONFIDENTIAL draft")},
                     same_app ? notepad_app() : browser_app());
    JobOptions options;
    options.selection = as_future("CONFIDENTIAL draft");
    auto job = std::make_shared<const DictationJob>(begin_job(
        JobMode::Command, *rig.settings.snapshot(), {}, std::move(options), &rig.capture));
    CHECK(job->focus == nullptr);

    REQUIRE(rig.runtime->claim_job(job));
    rig.runtime->on_transcription_complete("CONFIDENTIAL rewrite", "CONFIDENTIAL draft");
    rig.drain();

    CHECK(rig.pastes() == (same_app ? 1U : 0U));
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK_FALSE(entries[0].app_id.has_value());
    if (!same_app) {
        CHECK(rig.last_status() == CHANGED_STATUS);
    }
}

// from
// tests/test_personalize_fix_f2_paste_target.py::test_a_rewrite_whose_app_cannot_be_told_is_copied_not_pasted_blind
TEST_CASE("test_a_rewrite_whose_app_cannot_be_told_is_copied_not_pasted_blind",
          "[test_personalize_fix_f2_paste_target]") {
    CommandRig rig(unaware_options());
    rig.capture.set(std::make_shared<fv::focus::NullCaptureService>());

    REQUIRE(rig.runtime->claim_job(command_job("old words")));
    rig.runtime->on_transcription_complete("CONFIDENTIAL rewrite", "old words");
    rig.drain();

    CHECK(rig.pastes() == 0);
    CHECK(rig.copies() == std::vector<std::string>{"CONFIDENTIAL rewrite"});
    CHECK(rig.last_status() == UNKNOWN_STATUS);
}

// ---- tests/test_personalize_fix2_g1_paste_target_tab.py ----

// from
// tests/test_personalize_fix2_g1_paste_target_tab.py::test_a_transform_finished_after_a_tab_switch_is_copied_not_pasted
TEST_CASE("test_a_transform_finished_after_a_tab_switch_is_copied_not_pasted",
          "[test_personalize_fix2_g1_paste_target_tab]") {
    CommandRig rig(unaware_options());
    rig.model->answer("CONFIDENTIAL rewrite");
    const AppIdentity gmail = chrome("Inbox (12) - someone@gmail.com - Gmail - Google Chrome");
    const AppIdentity whatsapp = chrome("(3) WhatsApp - Google Chrome");
    rig.capture.fake(FocusSnapshot{gmail, selected("CONFIDENTIAL draft")}, whatsapp);

    run_transform(rig, "polish");

    CHECK(rig.pastes() == 0);
    CHECK(rig.copies() == std::vector<std::string>{"CONFIDENTIAL rewrite"});
    CHECK(rig.last_status() == CHANGED_STATUS);
}
