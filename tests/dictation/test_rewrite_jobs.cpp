#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "command_support.h"
#include "dictation/rewrite_jobs.h"

// Command Mode recordings and rewrite jobs on the transcription job slot
// (tests/test_personalize_foundation_runtime.py, the Command Mode and rewrite half).

using namespace fv::dictation;
using namespace fv::dictation::testing;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::FocusSnapshot;

namespace {

AppIdentity outlook() {
    return app("OUTLOOK.EXE", "Outlook", 7);
}

// h: auto-paste, copy and restore on; the engine hears "hello there".
CommandRigOptions foundation_options() {
    CommandRigOptions options;
    options.heard = "hello there";
    return options;
}

// _started_in_outlook: a target the paste check can confirm, Outlook then and now.
std::shared_ptr<fv::focus::FocusRequest> started_in_outlook(CommandRig& rig) {
    rig.capture.fake(FocusSnapshot{outlook(), {}}, outlook());
    return fv::focus::FocusRequest::resolved(FocusSnapshot{outlook(), {}});
}

std::shared_ptr<const DictationJob> job_of(JobMode mode,
                                           std::shared_ptr<fv::focus::FocusRequest> target = {},
                                           std::string source = {}) {
    DictationJob job;
    job.mode = mode;
    job.target = std::move(target);
    job.source = std::move(source);
    return std::make_shared<const DictationJob>(std::move(job));
}

// command_runtime as a Mock: answers or raises as the test says, recording its calls.
class FakeCompletion final : public CommandCompletion {
public:
    RewriteResult complete_recording(const std::string& raw,
                                     const std::shared_ptr<const DictationJob>& job) override {
        {
            std::lock_guard lock(mutex_);
            calls_.emplace_back(raw, job ? job->id : std::string());
        }
        if (error) {
            throw std::runtime_error(*error);
        }
        return result;
    }
    std::vector<std::pair<std::string, std::string>> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    RewriteResult result;
    std::optional<std::string> error;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, std::string>> calls_;
};

} // namespace

// from
// tests/test_personalize_foundation_runtime.py::test_command_recording_listens_and_takes_no_clipboard_prefetch
TEST_CASE("test_command_recording_listens_and_takes_no_clipboard_prefetch",
          "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    StartOptions options;
    options.mode = JobMode::Command;
    options.selection = as_future("the old words");

    REQUIRE(rig.runtime->start_recording(std::move(options)));

    const auto job = rig.recording_job();
    REQUIRE(job);
    CHECK(job->mode == JobMode::Command);
    CHECK(job->selection_text() == "the old words");
    REQUIRE_FALSE(rig.overlays().empty());
    CHECK(rig.overlays().back() == "command_listening");
    CHECK(rig.trace.count("prefetch") == 0);
}

// from
// tests/test_personalize_foundation_runtime.py::test_command_recordings_go_to_the_command_runtime
TEST_CASE("test_command_recordings_go_to_the_command_runtime",
          "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    FakeCompletion command;
    const CleanupInfo info{"openai", "gpt", 0.2, "medium"};
    command.result = RewriteResult{"Rewritten.", "old words", info};
    rig.runtime->set_command_runtime(&command);
    const auto job = job_of(JobMode::Command);

    REQUIRE(rig.runtime->claim_job(job));
    rig.transcribe();
    rig.runtime->set_command_runtime(rig.commands.get());

    CHECK(command.calls() ==
          std::vector<std::pair<std::string, std::string>>{{"hello there", job->id}});
    // Cleanup never ran: the model was not asked.
    CHECK(rig.model->requests().empty());
    const auto completed = rig.completed();
    REQUIRE(completed.size() == 1);
    CHECK(completed[0].text == "Rewritten.");
    CHECK(completed[0].raw == std::optional<std::string>("old words"));
    CHECK(completed[0].info == std::optional<CleanupInfo>(info));
}

// from tests/test_personalize_foundation_runtime.py::test_a_failing_command_reports_its_message
TEST_CASE("test_a_failing_command_reports_its_message", "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    FakeCompletion command;
    command.error = "Select the text to change first";
    rig.runtime->set_command_runtime(&command);

    REQUIRE(rig.runtime->claim_job(job_of(JobMode::Command)));
    rig.transcribe();
    rig.runtime->set_command_runtime(rig.commands.get());

    CHECK(rig.last_status() == "Error: Select the text to change first");
    CHECK(rig.pastes() == 0);
    CHECK_FALSE(rig.runtime->has_active_job());
}

// from tests/test_personalize_foundation_runtime.py::test_rewrites_paste_even_with_auto_paste_off
TEST_CASE("test_rewrites_paste_even_with_auto_paste_off", "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    rig.settings.update(Json{{"auto_paste", false}});

    REQUIRE(rig.runtime->claim_job(job_of(JobMode::Transform, started_in_outlook(rig))));
    rig.runtime->on_transcription_complete("Shorter.", "A much longer text.");
    rig.drain();
    CHECK(rig.pastes() == 1);

    REQUIRE(rig.runtime->claim_job(std::make_shared<const DictationJob>()));
    rig.runtime->on_transcription_complete("Dictated.");
    rig.drain();
    CHECK(rig.pastes() == 1);
    REQUIRE_FALSE(rig.copies().empty());
    CHECK(rig.copies().back() == "Dictated.");
}

// from
// tests/test_personalize_foundation_runtime.py::test_a_rewrite_is_only_copied_when_the_app_changed
TEST_CASE("test_a_rewrite_is_only_copied_when_the_app_changed",
          "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    rig.capture.fake(FocusSnapshot{outlook(), {}}, app("slack.exe", "Slack", 11));
    const auto job = std::make_shared<const DictationJob>(
        begin_job(JobMode::Command, Json::object(), {}, {}, &rig.capture));

    REQUIRE(rig.runtime->claim_job(job));
    rig.runtime->on_transcription_complete("Rewritten.");
    rig.drain();

    CHECK(rig.pastes() == 0);
    CHECK(rig.copies() == std::vector<std::string>{"Rewritten."});
    CHECK(rig.last_status() == "Rewrite copied — the app changed; paste it where you want");
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry_kind == std::optional<std::string>("command"));
}

// from
// tests/test_personalize_foundation_runtime.py::test_rewrite_jobs_are_refused_while_recording_or_busy
TEST_CASE("test_rewrite_jobs_are_refused_while_recording_or_busy",
          "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    const auto job = job_of(JobMode::Transform, {}, "hotkey");
    rig.recorder.recording = true;
    CHECK_FALSE(rig.runtime->begin_rewrite_job(job, "Polish"));

    rig.recorder.recording = false;
    REQUIRE(rig.runtime->claim_job());
    CHECK_FALSE(rig.runtime->begin_rewrite_job(job, "Polish"));
    CHECK_THAT(rig.last_status(), ContainsSubstring("wait before rewriting text"));
}

// from tests/test_personalize_foundation_runtime.py::test_a_submitted_rewrite_is_pasted_and_saved
TEST_CASE("test_a_submitted_rewrite_is_pasted_and_saved", "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    const auto job = job_of(JobMode::Transform, started_in_outlook(rig));

    REQUIRE(rig.runtime->begin_rewrite_job(job, "Polish"));
    CHECK(rig.runtime->has_active_job());
    CHECK(rig.runtime->job_slot().active_job() == job);
    REQUIRE_FALSE(rig.overlays().empty());
    CHECK(rig.overlays().back() == "rewriting");

    rig.runtime->submit_rewrite(
        [] { return RewriteResult{"Polished.", "rough text", std::nullopt}; });
    rig.drain();

    CHECK(rig.pastes() == 1);
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text == "Polished.");
    CHECK(entries[0].raw_text == std::optional<std::string>("rough text"));
    CHECK(entries[0].source_name == std::optional<std::string>("Polish"));
    CHECK(entries[0].entry_kind == std::optional<std::string>("transform"));
    CHECK_FALSE(rig.runtime->has_active_job());
}

// from tests/test_personalize_foundation_runtime.py::test_a_failed_rewrite_frees_the_slot
TEST_CASE("test_a_failed_rewrite_frees_the_slot", "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    REQUIRE(rig.runtime->begin_rewrite_job(job_of(JobMode::Transform), "Polish"));

    rig.runtime->submit_rewrite(
        []() -> RewriteResult { throw std::runtime_error("No AI provider is set up"); });
    rig.drain();

    CHECK(rig.last_status() == "Error: No AI provider is set up");
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.entries().empty());
}

// from tests/test_personalize_foundation_runtime.py::test_an_abandoned_rewrite_frees_the_slot_once
TEST_CASE("test_an_abandoned_rewrite_frees_the_slot_once",
          "[test_personalize_foundation_runtime]") {
    CommandRig rig(foundation_options());
    REQUIRE(rig.runtime->begin_rewrite_job(job_of(JobMode::Transform), "Polish"));

    rig.runtime->abandon_rewrite_job("Select the text to change first");

    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.last_status() == "Select the text to change first");
    REQUIRE_FALSE(rig.overlays().empty());
    CHECK(rig.overlays().back() == "none");

    // Not a rewrite's slot any more: a later abandon leaves other jobs alone.
    REQUIRE(rig.runtime->claim_job());
    rig.runtime->abandon_rewrite_job("late");
    CHECK(rig.runtime->has_active_job());
}
