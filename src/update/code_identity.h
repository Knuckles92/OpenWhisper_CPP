#pragma once

#include <filesystem>
#include <optional>
#include <string>

// Whether the program on disk is still the one this process started from
// (services/code_identity.py). An upgrade that replaces the installed executable under a running
// app leaves it mixing what it loaded with files from the new version; only a restart puts them
// back in step. A FioriVoce build never runs from a Python checkout, so only the installed
// executable counts.
namespace fv::update {

// "<file index>:<size>:<modification time in ns>" of the executable, as os.stat reads it;
// nullopt when it cannot be read. Opens the file for its attributes only and never starts a
// process: safe on the GUI thread.
std::optional<std::string> code_identity(const std::filesystem::path& exe);

// Affinity: thread-safe.
class CodeIdentity {
public:
    // Records `exe` and its identity now, as the program this process runs (before the GUI
    // starts).
    static void remember_launch(const std::filesystem::path& exe);
    // True once the program on disk is no longer the one remembered; false when nothing was
    // remembered or either identity is unknown.
    static bool changed_since_launch();
    // Forgets the launch (tests).
    static void forget_launch();
};

} // namespace fv::update
