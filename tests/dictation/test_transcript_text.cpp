// How the Upload File tab reads a job's transcript (ui_qt/widgets/upload_file_tab.py
// set_transcript and show_job_error).
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "dictation/messages.h"
#include "dictation/transcript_text.h"

using namespace fv::dictation;

TEST_CASE("an error transcript is failed and never copyable") {
    CHECK(is_failed("Error: the file could not be read"));
    CHECK(is_failed("  \nError: x"));
    CHECK_FALSE(is_failed("error: lower case is a transcript"));
    CHECK_FALSE(is_failed("The word Error: inside"));
    CHECK_FALSE(is_copyable("Error: x"));
}

TEST_CASE("only a failure that says canceled is canceled") {
    CHECK(is_canceled("Error: Transcription canceled"));
    CHECK(is_canceled("Error: CANCELED by the user"));
    CHECK_FALSE(is_canceled("Error: timed out"));
    // Not a failure, so not a cancel, whatever it says.
    CHECK_FALSE(is_canceled("I canceled the meeting"));
}

TEST_CASE("blank text and the empty decode are not copyable") {
    CHECK_FALSE(is_copyable(""));
    CHECK_FALSE(is_copyable(" \n\t"));
    CHECK_FALSE(is_copyable(std::string(EMPTY_ASR_MESSAGE)));
    CHECK_FALSE(is_copyable("  " + std::string(EMPTY_ASR_MESSAGE) + "\n"));
    CHECK(is_copyable("Hello there."));
    CHECK(is_copyable(" " + std::string(EMPTY_ASR_MESSAGE) + " and more"));
}

TEST_CASE("a job error says why without its prefix") {
    CHECK(job_error_reason("Error: The file is too large. ") == "The file is too large.");
    CHECK(job_error_reason("Something broke") == "Something broke");
    CHECK(job_error_reason("  Error: kept") == "Error: kept");
    CHECK(job_error_reason("Error: ").empty());
    CHECK(is_canceled_error("Transcription Canceled"));
    CHECK(is_canceled_error("Error: canceled"));
    CHECK_FALSE(is_canceled_error("Error: failed"));
}
