#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "automation_support.h"
#include "dictation/command_selection.h"
#include "dictation/events.h"
#include "dictation/messages.h"

// An action's words survive shaping problems, and events never carry a selection
// (tests/test_automation_fix_pipeline_shaping.py): a transform-shaped action whose model refused
// or failed, an action deleted while recording, and a routed dictation into either, keep their
// words and their History entry. The transform runs through a fake CommandCompletion where
// OpenWhisper monkeypatches text_rewrite and command_runtime.rewrite_text.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

const std::string kHeard = "ask claude what time it is";
const std::string kCleaned = "ASK CLAUDE WHAT TIME IT IS";
const std::string kPolishFailed = "Polish couldn't run — sent your cleaned words";

Json polished() {
    return action_json("a3", "Prompt Claude", "scripts", Json::object(), "transform:polish");
}
Json formal() {
    return action_json("a4", "Formal note", "scripts", Json::object(), "profile:email");
}
Json email_profile() {
    return Json{{"id", "email"}, {"name", "Email"}, {"instructions", "Write it as an email."}};
}
Json polish_transform() {
    return Json{{"id", "polish"}, {"name", "Polish"}, {"instruction", "Polish it."}};
}

// The rig with the saved transform the shaped action names.
AutomationRigOptions with_polish() {
    AutomationRigOptions options;
    options.settings["text_transforms"] = Json::array({polish_transform()});
    return options;
}

FakeAutomationPort::Delivery delivered(const AutomationRig& rig) {
    const auto all = rig.port.deliveries();
    REQUIRE(all.size() == 1);
    return all.front();
}

void never_mentions_command_mode(const AutomationRig& rig) {
    for (const auto& text : rig.statuses()) {
        CHECK(text.find("Command Mode") == std::string::npos);
    }
    for (const auto& text : rig.notices()) {
        CHECK(text.find("Command Mode") == std::string::npos);
    }
}

// Whether the recording's WAV was kept in Recordings, as _preserve_failed_audio keeps it.
bool audio_kept(const AutomationRig& rig) {
    for (const auto& status : rig.statuses()) {
        if (status.find(" — audio saved in Recordings as ") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("test_a_transform_without_an_ai_provider_sends_the_cleaned_words",
          "[test_automation_fix_pipeline_shaping]") {
    AutomationRig rig(with_polish());
    rig.with_actions({polished()});
    rig.cleaner.available = false;
    // CommandRuntime.rewrite_text without a provider.
    rig.rewriter.rewrite = [](const std::string&, const std::string&) -> RewriteResult {
        throw CommandRefused("Set up AI cleanup to use transforms");
    };
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Action, "a3")));
    rig.runtime->transcribe_audio_file(rig.wav());
    rig.run_workers();

    const auto delivery = delivered(rig);
    CHECK(delivery.job->action_id == "a3");
    CHECK(delivery.delivery.text == kHeard);
    CHECK(delivery.delivery.notice == kPolishFailed);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == kHeard);
    CHECK(entries[0].entry_kind == "action");
    CHECK_FALSE(audio_kept(rig));
    CHECK(rig.events.of("transcription.failed").empty());
    CHECK_FALSE(rig.runtime->has_active_job());
    never_mentions_command_mode(rig);
}

TEST_CASE("test_a_transform_the_model_fails_or_refuses_sends_the_cleaned_words",
          "[test_automation_fix_pipeline_shaping]") {
    const bool refusal = GENERATE(false, true);
    CAPTURE(refusal);
    AutomationRig rig(with_polish());
    rig.with_actions({polished()});
    rig.rewriter.rewrite = [refusal](const std::string&, const std::string&) -> RewriteResult {
        if (refusal) {
            // text_rewrite.TOO_LONG_MESSAGE: a refusal.
            throw CommandRefused("That's too much text to rewrite at once.");
        }
        throw std::runtime_error("The AI model took too long. Try a faster one in AI cleanup");
    };
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Action, "a3")));
    rig.runtime->transcribe_audio_file(rig.wav());
    rig.run_workers();

    const auto delivery = delivered(rig);
    CHECK(delivery.delivery.text == kCleaned); // cleanup ran; the transform didn't
    CHECK(delivery.delivery.notice == kPolishFailed);
    CHECK(rig.entries().size() == 1);
    CHECK_FALSE(audio_kept(rig));
}

TEST_CASE("test_an_action_deleted_while_recording_still_gets_the_words_and_history",
          "[test_automation_fix_pipeline_shaping]") {
    AutomationRig rig;
    rig.live_recording();
    const auto job = make_job(JobMode::Action, "deleted-meanwhile", "hotkey");
    REQUIRE(rig.runtime->claim_job(job));
    rig.runtime->transcribe_audio_file(rig.wav());
    rig.run_workers();

    // The action runtime says it no longer exists and copies the words.
    const auto delivery = delivered(rig);
    CHECK(delivery.job->id == job->id);
    CHECK(delivery.delivery.text == kCleaned);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == kCleaned);
    CHECK_FALSE(audio_kept(rig));
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("test_a_routed_dictation_into_a_refusing_transform_is_not_lost",
          "[test_automation_fix_pipeline_shaping]") {
    AutomationRig rig(with_polish());
    rig.with_actions({polished()});
    rig.rewriter.rewrite = [](const std::string&, const std::string&) -> RewriteResult {
        throw CommandRefused("Set up AI cleanup to use transforms");
    };
    rig.port.decide = [](const std::string&, const std::string&) {
        return RouteDecision{"a3", "what time it is", 0.9, ""};
    };
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Dictation, "", "", "auto")));
    rig.runtime->transcribe_audio_file(rig.wav());
    rig.run_workers();

    const auto delivery = delivered(rig);
    CHECK(delivery.job->routed);
    CHECK(delivery.delivery.text == "WHAT TIME IT IS");
    CHECK(delivery.delivery.notice == kPolishFailed);
    CHECK(rig.entries().size() == 1);
    CHECK_FALSE(audio_kept(rig));
}

TEST_CASE("test_a_cancel_during_the_transform_is_still_a_cancel",
          "[test_automation_fix_pipeline_shaping]") {
    AutomationRig rig(with_polish());
    rig.with_actions({polished()});
    rig.rewriter.rewrite = [&rig](const std::string&, const std::string&) -> RewriteResult {
        rig.runtime->cancel();
        throw std::runtime_error("Transcription canceled");
    };
    rig.transcribe(make_job(JobMode::Action, "a3"));
    rig.run_workers();

    CHECK(rig.port.deliveries().empty());
    CHECK(rig.entries().empty());
}

// Python calls _refuse directly; here a refusal reaches the worker as a CommandRefused from the
// cleanup pass, which is where the transcription runtime turns one into _refuse.
TEST_CASE("test_a_refusal_never_drops_a_dictation_or_action_recording",
          "[test_automation_fix_pipeline_shaping]") {
    const JobMode mode = GENERATE(JobMode::Action, JobMode::Dictation);
    CAPTURE(to_string(mode));
    AutomationRig rig;
    rig.cleaner.during = [] {
        throw CommandRefused("Nothing to do");
    };
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(mode, "a1")));
    rig.runtime->transcribe_audio_file(rig.recorder.output_file());
    rig.run_workers();

    CHECK(rig.recorder.acknowledged == 1); // only once Recordings holds a copy
    CHECK(audio_kept(rig));
    CHECK(rig.statuses().back().starts_with("Nothing to do — audio saved in Recordings as "));
}

TEST_CASE("test_a_command_refusal_still_drops_its_instruction_audio",
          "[test_automation_fix_pipeline_shaping]") {
    AutomationRig rig;
    rig.rewriter.refuse_recording = "Select text first";
    rig.live_recording();
    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Command)));
    rig.runtime->transcribe_audio_file(rig.recorder.output_file());
    rig.run_workers();

    CHECK(rig.recorder.acknowledged == 1);
    CHECK_FALSE(audio_kept(rig));
    CHECK(rig.last_status() == "Select text first");
}

TEST_CASE("test_an_api_run_profile_action_says_its_formatting_failed",
          "[test_automation_fix_pipeline_shaping]") {
    AutomationRig rig;
    rig.with_actions({formal()});
    rig.settings.update(Json{{"transcript_cleanup_profiles", Json::array({email_profile()})}});
    rig.cleaner.error = "timeout";
    rig.runtime->run_action_text("a4", "hello there", "api");
    rig.run_workers();

    const auto delivery = delivered(rig);
    CHECK(delivery.delivery.text == "hello there");
    CHECK(delivery.delivery.notice == "Email formatting failed (timeout); using raw transcript");
}

TEST_CASE("test_an_action_without_problems_has_no_notice",
          "[test_automation_fix_pipeline_shaping]") {
    AutomationRig rig;
    rig.transcribe(make_job(JobMode::Action, "a1"));
    rig.run_workers();
    CHECK(delivered(rig).delivery.notice.empty());
}

// Python sets the recording's profile on the runtime; here the recording starts with it.
TEST_CASE("test_a_profile_dictation_still_shows_the_failure_in_its_status",
          "[test_automation_fix_pipeline_shaping]") {
    AutomationRig rig;
    rig.settings.update(Json{{"transcript_cleanup_profiles", Json::array({email_profile()})}});
    rig.cleaner.error = "timeout";
    StartOptions options;
    options.profile_id = "email";
    REQUIRE(rig.runtime->start_recording(options));
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(rig.statuses().at(rig.statuses().size() - 1) ==
          "Ready (Pasted) — Email formatting failed (timeout); using raw transcript");
}

TEST_CASE("test_a_rewrite_publishes_no_selected_text", "[test_automation_fix_pipeline_shaping]") {
    const JobMode mode = GENERATE(JobMode::Transform, JobMode::Command);
    CAPTURE(to_string(mode));
    const std::string secret = "Dear Sam, my bank PIN is 4512 and the door code is 0091.";
    AutomationRig rig;
    auto job = std::make_shared<DictationJob>();
    job->mode = mode;
    job->selection = as_future(secret);
    REQUIRE(rig.runtime->begin_rewrite_job(job, "Transform · Polish"));
    // CommandRuntime's rewrite returns the selection as raw_text.
    rig.runtime->submit_rewrite(
        [secret] { return RewriteResult{"Dear Sam, here are the codes.", secret, std::nullopt}; });
    rig.run_workers();

    const auto completed = rig.events.of("transcription.completed");
    REQUIRE(completed.size() == 1);
    CHECK(completed[0].data["raw_text"].is_null());
    CHECK(completed[0].data.dump().find(secret) == std::string::npos);
    // History keeps the original, as before.
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].raw_text == secret);
}

TEST_CASE("test_dictations_actions_and_files_keep_their_before_cleanup_text",
          "[test_automation_fix_pipeline_shaping]") {
    for (const JobMode mode : {JobMode::Dictation, JobMode::Action}) {
        DictationJob job;
        job.mode = mode;
        const Json data = events::transcription_completed(
            &job, "Hello.", std::string("hello"), Json::object(), std::nullopt, std::nullopt);
        CHECK(data["raw_text"] == "hello");
    }
    const Json data = events::transcription_completed(nullptr, "Hello.", std::string("hello"),
                                                      Json::object(), std::nullopt, std::nullopt);
    CHECK(data["mode"] == "file");
    CHECK(data["raw_text"] == "hello");
}
