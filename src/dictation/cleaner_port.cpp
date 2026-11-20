#include "dictation/cleaner_port.h"

#include <format>

namespace fv::dictation {

bool NullTranscriptCleaner::is_available(const Json&) {
    return false;
}

CleanupResult NullTranscriptCleaner::cleanup(const CleanupRequest& request) {
    CleanupResult result;
    result.text = request.text;
    result.error = std::string(CLEANUP_UNAVAILABLE);
    return result;
}

TranscriptCleaner& null_transcript_cleaner() {
    static NullTranscriptCleaner cleaner;
    return cleaner;
}

std::string cleanup_status_suffix(const std::optional<std::string>& cleanup_error) {
    if (!cleanup_error || cleanup_error->empty()) {
        return {};
    }
    if (*cleanup_error == CLEANUP_CANCELED_REASON) {
        // Only a Cancel cuts the cleanup short.
        return " without AI cleanup";
    }
    return std::format(" — AI cleanup failed ({}); showing raw text", *cleanup_error);
}

} // namespace fv::dictation
