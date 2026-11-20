#include "dictation/transcript_text.h"

#include "dictation/messages.h"
#include "pyport/text.h"

namespace fv::dictation {

namespace {

bool says_canceled(std::string_view text) {
    return py::lower(text).find("canceled") != std::string::npos;
}

} // namespace

bool is_failed(std::string_view transcript) {
    return py::strip(transcript).starts_with("Error:");
}

bool is_canceled(std::string_view transcript) {
    const std::string stripped = py::strip(transcript);
    return stripped.starts_with("Error:") && says_canceled(stripped);
}

bool is_copyable(std::string_view transcript) {
    const std::string stripped = py::strip(transcript);
    return !stripped.empty() && stripped != EMPTY_ASR_MESSAGE && !stripped.starts_with("Error:");
}

bool is_canceled_error(std::string_view message) {
    return says_canceled(message);
}

std::string job_error_reason(std::string_view message) {
    constexpr std::string_view prefix = "Error: ";
    if (message.starts_with(prefix)) {
        message.remove_prefix(prefix.size());
    }
    return py::strip(message);
}

} // namespace fv::dictation
