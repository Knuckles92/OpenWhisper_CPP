#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "dictation/transcription_runtime.h"
#include "runtime_support.h"

// tests/test_personalize_fix3_h1_outcome_notices.py, the dictation part: a plain dictation's
// outcome stays on the status line; only rewrites and refusals, run from other apps, show near
// the pointer.

using namespace fv::dictation;
using namespace fv::dictation::testing;

namespace {

std::shared_ptr<const DictationJob> dictation_job() {
    DictationJob job;
    job.mode = JobMode::Dictation;
    return std::make_shared<const DictationJob>(std::move(job));
}

} // namespace

TEST_CASE("plain dictation gets no notice", "[test_personalize_fix3_h1_outcome_notices]") {
    // from tests/test_personalize_fix3_h1_outcome_notices.py::test_plain_dictation_gets_no_notice
    RigOptions options;
    options.script.texts = {"Hello."};
    RuntimeRig rig(options);
    const auto audio = rig.wav("audio.wav");
    REQUIRE(rig.runtime->claim_job(dictation_job()));
    rig.runtime->transcribe_audio_file(audio);
    rig.run_workers();
    CHECK(rig.clipboard.inner.stages.size() == 1);
    CHECK(rig.keys.sent() == std::vector<std::string>{"paste"});

    REQUIRE(rig.runtime->claim_job(dictation_job()));
    rig.runtime->on_transcription_error("The engine crashed");
    rig.run_workers();

    CHECK(rig.notices().empty());
}

TEST_CASE("a failed rewrite shows its status near the pointer, unless canceled",
          "[dictation][notices]") {
    RuntimeRig rig;
    DictationJob command;
    command.mode = JobMode::Command;
    REQUIRE(rig.runtime->claim_job(std::make_shared<const DictationJob>(command)));
    rig.runtime->on_transcription_error("Command Mode isn't available");
    CHECK(rig.notices() == std::vector<std::string>{"Error: Command Mode isn't available"});

    REQUIRE(rig.runtime->claim_job(std::make_shared<const DictationJob>(command)));
    rig.runtime->job_slot().cancel();
    rig.runtime->on_transcription_error("Transcription canceled");
    CHECK(rig.notices().size() == 1);
}
