#include "speech/service.h"

namespace fv::speech {

bool Backend::warm_up(const CancelToken&) {
    return false;
}

std::string Backend::recognition_support() const {
    return "after";
}

std::optional<double> Backend::large_file_size_mb(const std::filesystem::path&) const {
    return std::nullopt;
}

bool Backend::supports_recognition() const {
    return false;
}

bool Backend::prepares_for_transcription() const {
    return false;
}

void Backend::prepare_for_transcription() {}

void Backend::report_progress(const DecodeJob& job, const std::string& message,
                              bool transcribing) const {
    if (job.on_progress) {
        job.on_progress(message, transcribing);
    }
    progress.emit(message, transcribing);
}

} // namespace fv::speech
