// tests/test_personalize_s4_recognition.py: a recognition context (language and dictionary
// phrases) reaches only a backend that takes one, and a Parakeet dictation gets its phrases in the
// job and its "sounds like" replacements in the text. Parakeet's phrases never reach NeMo (lead
// decision 2), so the engine is asked only for the language.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "cleanup_support.h"
#include "dictation/recognition.h"
#include "runtime_support.h"

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

const std::vector<std::string> kPhrases{"Ksenia", "Kubernetes"};

Json dictionary() {
    return Json::array(
        {Json{
             {"id", "a"}, {"term", "Ksenia"}, {"heard", Json::array({"Sonia"})}, {"starred", true}},
         Json{{"id", "b"}, {"term", "Kubernetes"}}});
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

} // namespace

TEST_CASE("empty or unsupported calls as before", "[test_personalize_s4_recognition]") {
    // from
    // tests/test_personalize_s4_recognition.py::TestTranscribeSeam::test_empty_or_unsupported_calls_as_before
    CHECK_FALSE(context_for_backend(std::nullopt, true));
    CHECK_FALSE(context_for_backend(RecognitionContext{}, true));
    CHECK_FALSE(context_for_backend(RecognitionContext{"", kPhrases}, false));
    CHECK_FALSE(context_for_backend(RecognitionContext{"de", {}}, false));
}

TEST_CASE("a context reaches a backend that takes one", "[test_personalize_s4_recognition]") {
    // from
    // tests/test_personalize_s4_recognition.py::TestTranscribeSeam::test_a_context_reaches_a_backend_that_takes_one
    const RecognitionContext recognition{"fr", kPhrases};

    CHECK(context_for_backend(recognition, true) == recognition);
}

TEST_CASE("a dictation reaches whisper with hints and replacements",
          "[test_personalize_s4_recognition]") {
    // from
    // tests/test_personalize_s4_recognition.py::test_a_dictation_reaches_whisper_with_hints_and_replacements
    // On Parakeet (no Local Whisper here): the saved dictionary gives the job its phrases and fixes
    // the text after recognition.
    RigOptions options;
    options.settings["dictation_dictionary"] = dictionary();
    options.script.texts = {"Ask Sonia about the cluster"};
    RuntimeRig rig(options);
    std::vector<std::tuple<std::string, std::optional<std::string>, std::optional<CleanupInfo>>>
        completed;
    const auto link = rig.runtime->transcription_completed.connect(
        rig.ui, [&](std::string text, std::optional<std::string> raw,
                    std::optional<CleanupInfo> info) { completed.emplace_back(text, raw, info); });
    const Json settings = *rig.settings.snapshot();
    const auto job = std::make_shared<const DictationJob>(begin_job(
        JobMode::Dictation, settings, recognition_for(nullptr, settings), {}, &rig.capture));
    REQUIRE(rig.runtime->claim_job(job));

    rig.runtime->transcribe_audio_file(rig.wav("a.wav"));
    rig.run_workers();

    REQUIRE(job->recognition);
    CHECK(job->recognition->phrases == kPhrases);
    REQUIRE(completed.size() == 1);
    CHECK(std::get<0>(completed[0]) == "Ask Ksenia about the cluster");
    CHECK_FALSE(std::get<1>(completed[0]));
    CHECK_FALSE(std::get<2>(completed[0]));
}

TEST_CASE("language comes from the context per request", "[test_personalize_s4_recognition]") {
    // from
    // tests/test_personalize_s4_recognition.py::TestLocalSpeech::test_language_comes_from_the_context_per_request
    // Each final pass decodes with its job's language; a file without a job keeps the engine's
    // own setting ("").
    RigOptions options;
    options.settings["selected_model"] = "parakeet";
    options.settings["dictation_languages"] = Json::array({"en", "fr"});
    options.settings["dictation_active_language"] = "fr";
    options.settings["dictation_dictionary"] = dictionary();
    RuntimeRig rig(options);
    const auto dictate = [&rig] {
        REQUIRE(rig.runtime->claim_job(
            shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false))));
        rig.runtime->transcribe_audio_file(rig.wav("a.wav"));
        rig.run_workers();
    };

    dictate();
    rig.settings.update(Json{{"dictation_active_language", "en"}});
    dictate();
    rig.runtime->transcribe_audio_file(rig.wav("upload.wav"));
    rig.run_workers();

    CHECK(decoded_with(rig) == std::vector<std::string>{"fr", "en", ""});
}

TEST_CASE("an engine that takes no context decodes as without one", "[dictation][recognition]") {
    // recognition_context.transcribe: the loaded fake does not take a context and "api" is no
    // local engine, so the job's language never reaches the decode.
    RigOptions options;
    options.settings["selected_model"] = "api";
    options.settings["dictation_languages"] = Json::array({"en", "fr"});
    options.settings["dictation_active_language"] = "fr";
    options.settings["dictation_dictionary"] = dictionary();
    RuntimeRig rig(options);
    const Json settings = *rig.settings.snapshot();
    const auto job = std::make_shared<const DictationJob>(begin_job(
        JobMode::Dictation, settings, recognition_for(nullptr, settings), {}, &rig.capture));
    REQUIRE(job->recognition);
    REQUIRE(job->recognition->language == "fr");
    REQUIRE(rig.runtime->claim_job(job));

    rig.runtime->transcribe_audio_file(rig.wav("a.wav"));
    rig.run_workers();

    CHECK(decoded_with(rig) == std::vector<std::string>{""});
}

TEST_CASE("no language leaves the engine setting", "[test_personalize_s4_recognition]") {
    // from
    // tests/test_personalize_s4_recognition.py::TestLocalSpeech::test_no_language_leaves_the_engine_setting
    RigOptions options;
    options.settings["dictation_dictionary"] = dictionary();
    RuntimeRig rig(options);
    const auto job = shared(job_in(std::nullopt, std::nullopt, JobMode::Dictation, false));
    REQUIRE(rig.runtime->claim_job(job));

    rig.runtime->transcribe_audio_file(rig.wav("a.wav"));
    rig.run_workers();

    CHECK(recognition_for(nullptr, *rig.settings.snapshot()) == RecognitionContext{"", kPhrases});
    CHECK(decoded_with(rig) == std::vector<std::string>{""});
}
