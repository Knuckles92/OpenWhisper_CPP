// The options every decode sends (services/local_asr/nvidia.py _options), without a runtime.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "engine/request_options.h"

namespace {

nemo_speech_asr_recognition_options defaults() {
    nemo_speech_asr_recognition_options options{};
    options.size = sizeof(options);
    options.request_id = "kept";
    options.max_alternatives = 3;
    options.verbatim_transcripts = true;
    return options;
}

} // namespace

TEST_CASE("word offsets, punctuation and interim results are on for every request") {
    const std::string language = "en";
    const auto options = fv::request_options(defaults(), language);
    CHECK(options.enable_word_time_offsets);
    CHECK(options.enable_automatic_punctuation);
    CHECK(options.interim_results);
}

TEST_CASE("the runtime's other defaults are kept") {
    const std::string language = "en";
    const auto options = fv::request_options(defaults(), language);
    CHECK(options.size == sizeof(options));
    CHECK(std::strcmp(options.request_id, "kept") == 0);
    CHECK(options.max_alternatives == 3);
    CHECK(options.verbatim_transcripts);
}

TEST_CASE("auto and an empty language send no language code") {
    const std::string automatic = "auto";
    const std::string empty;
    CHECK(fv::request_options(defaults(), automatic).language_code == nullptr);
    CHECK(fv::request_options(defaults(), empty).language_code == nullptr);
    CHECK(fv::native_language_code(automatic) == nullptr);
}

TEST_CASE("a language code points into the caller's string") {
    const std::string language = "fr";
    const auto options = fv::request_options(defaults(), language);
    CHECK(options.language_code == language.c_str());
    // Only the exact "auto" means detection; other spellings go to the runtime as they are.
    const std::string upper = "AUTO";
    CHECK(fv::native_language_code(upper) == upper.c_str());
}
