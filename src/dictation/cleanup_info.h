#pragma once

#include <string>

namespace fv::dictation {

// services/transcript_cleanup.py CleanupInfo: what a cleanup pass that changed the words used.
// Only a pass that ran makes one; AI cleanup itself arrives with fv_llm (M4).
// Affinity: value type.
struct CleanupInfo {
    std::string provider;
    std::string model;
    // How long the provider took, in seconds.
    double elapsed_s = 0.0;
    // pipeline.h cleanup_level: "profile", "custom", "light", "medium", "high" or "".
    std::string level;

    bool operator==(const CleanupInfo&) const = default;
};

} // namespace fv::dictation
