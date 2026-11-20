#include "dictation/recovery.h"

#include <exception>
#include <format>

#include "base/log.h"
#include "recorder/journal.h"
#include "walkthrough/library.h"

namespace fv::dictation {

namespace {
constexpr log::Logger kLog{"services.runtime.transcription"};
} // namespace

std::string recovery_status(std::size_t recordings, std::size_t walkthroughs) {
    if (walkthroughs != 0) {
        return std::format("Recovered {} walkthrough(s) — open Saved Walkthroughs to finish them",
                           walkthroughs);
    }
    if (recordings != 0) {
        return std::format("Recovered {} interrupted recording(s) in Recordings", recordings);
    }
    return {};
}

namespace {

std::size_t recover_recordings_logged(const std::filesystem::path& output_file,
                                      const std::filesystem::path& recordings_folder) {
    try {
        return fv::recover_recordings(output_file, recordings_folder).size();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Recording recovery failed");
    }
    return 0;
}

} // namespace

std::string recover_interrupted_recordings(const std::filesystem::path& output_file,
                                           const std::filesystem::path& recordings_folder) {
    return recovery_status(recover_recordings_logged(output_file, recordings_folder), 0);
}

std::string recover_interrupted_recordings(const std::filesystem::path& output_file,
                                           const std::filesystem::path& recordings_folder,
                                           const std::filesystem::path& captures_root) {
    // The recordings first: a walkthrough's audio is one of the WAVs they recover.
    const std::size_t recovered = recover_recordings_logged(output_file, recordings_folder);
    std::size_t walkthroughs = 0;
    try {
        walkthroughs = static_cast<std::size_t>(
            walkthrough::attach_recovered_audio(captures_root, recordings_folder));
    } catch (const std::exception& error) {
        kLog.error_with(error, "Walkthrough recovery failed");
    }
    try {
        walkthrough::prune(captures_root);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not prune expired walkthroughs");
    }
    return recovery_status(recovered, walkthroughs);
}

} // namespace fv::dictation
