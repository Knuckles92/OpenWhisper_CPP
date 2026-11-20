#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "dictation/job_slot.h"
#include "dictation/recognition.h"
#include "dictation/transcription_runtime.h"
#include "runtime_support.h"
#include "speech/dictation_language.h"

// tests/test_personalize_fix_f5_final_language.py: History and Stats record the language the
// final pass decoded with. OpenWhisper's test dictates English and German on Whisper; Parakeet
// takes English and French, so French stands in for German.

using namespace fv::dictation;
using namespace fv::dictation::testing;

namespace {

RigOptions dictating() {
    RigOptions options;
    options.settings["selected_model"] = "parakeet";
    options.settings["dictation_languages"] = fv::Json::array({"en", "fr"});
    options.settings["dictation_active_language"] = "en";
    options.script.texts = {"Bonjour tout le monde"};
    return options;
}

std::vector<std::string> decoded_with(const RuntimeRig& rig) {
    std::vector<std::string> languages;
    for (const auto& call : rig.engine->calls()) {
        if (call.op == "decode") {
            languages.push_back(call.language);
        }
    }
    return languages;
}

void stop_and_transcribe(RuntimeRig& rig) {
    rig.runtime->stop_recording();
    rig.run_workers();
}

} // namespace

TEST_CASE("a language switched during the recording is the one recorded",
          "[test_personalize_fix_f5_final_language]") {
    // from
    // tests/test_personalize_fix_f5_final_language.py::test_a_language_switched_during_the_recording_is_the_one_recorded
    RuntimeRig rig(dictating());
    REQUIRE(rig.runtime->start_recording());
    REQUIRE(rig.runtime->current_job()->recognition->language == "en");
    rig.settings.mutate(fv::speech::cycle);

    stop_and_transcribe(rig);

    CHECK(decoded_with(rig) == std::vector<std::string>{"fr"});
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].language == "fr");
    const auto rows = rig.stats->rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].language == "fr");
}

TEST_CASE("the starting language is recorded without a switch",
          "[test_personalize_fix_f5_final_language]") {
    // from
    // tests/test_personalize_fix_f5_final_language.py::test_the_starting_language_is_recorded_without_a_switch
    RuntimeRig rig(dictating());
    REQUIRE(rig.runtime->start_recording());

    stop_and_transcribe(rig);

    CHECK(decoded_with(rig) == std::vector<std::string>{"en"});
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].language == "en");
}

TEST_CASE("a slot released meanwhile is never given its job back",
          "[test_personalize_fix_f5_final_language]") {
    // from
    // tests/test_personalize_fix_f5_final_language.py::test_a_slot_released_meanwhile_is_never_given_its_job_back
    // Python releases the slot inside _final_pass_recognition, between the claim and the swap of
    // the updated job; the swap is JobSlot::replace_active_job, driven here at that moment.
    JobSlot slot;
    DictationJob started;
    started.recognition = RecognitionContext{"en", {}};
    const auto job = std::make_shared<const DictationJob>(started);
    REQUIRE(slot.claim(job));
    auto switched = std::make_shared<DictationJob>(*job);
    switched->recognition = RecognitionContext{"fr", {}};

    slot.finish(); // released meanwhile

    CHECK_FALSE(slot.replace_active_job(job.get(), switched));
    CHECK(slot.active_job() == nullptr);
    CHECK_FALSE(slot.has_active_job());

    // While the slot still works on the job, the swap goes through.
    REQUIRE(slot.claim(job));
    CHECK(slot.replace_active_job(job.get(), switched));
    CHECK(slot.active_job() == switched);
    // Never onto another job.
    CHECK_FALSE(slot.replace_active_job(job.get(), job));
}

TEST_CASE("recognition_for takes the job language and never throws", "[dictation][recognition]") {
    fv::Json settings = dictating().settings;
    CHECK(recognition_for(nullptr, settings) == RecognitionContext{"en", {}});
    settings["dictation_active_language"] = "de"; // not one Parakeet takes
    CHECK(recognition_for(nullptr, settings) == RecognitionContext{"", {}});
    CHECK(recognition_for(nullptr, fv::Json()) == RecognitionContext{"", {}});
}
