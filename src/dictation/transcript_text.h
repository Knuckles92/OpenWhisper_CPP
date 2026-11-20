#pragma once

#include <string>
#include <string_view>

// How the Upload File tab reads the transcript a job hands it (ui_qt/widgets/upload_file_tab.py
// set_transcript and show_job_error): the runtime reports failures as "Error: ..." text and an
// empty decode as EMPTY_ASR_MESSAGE, and neither is a transcript to copy. Every function here is
// thread-safe.
namespace fv::dictation {

// The stripped text starts with "Error:".
bool is_failed(std::string_view transcript);
// A failure whose text says "canceled" (any case).
bool is_canceled(std::string_view transcript);
// Something to copy: not blank, not EMPTY_ASR_MESSAGE, not a failure.
bool is_copyable(std::string_view transcript);

// show_job_error: whether a job's error message says "canceled" (any case).
bool is_canceled_error(std::string_view message);
// show_job_error: the message without a leading "Error: ", stripped; the card's error note.
std::string job_error_reason(std::string_view message);

} // namespace fv::dictation
