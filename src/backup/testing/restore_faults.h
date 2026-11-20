#pragma once

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

// Faults a test injects into apply_pending_restore, where OpenWhisper's tests monkeypatch
// os.replace and Path.unlink. One test at a time; an empty hook does nothing.
namespace fv::backup::testing {

struct RestoreFaults {
    // Called before each move of a restore (_move_path); throwing makes that move fail.
    std::function<void(const std::filesystem::path& source,
                       const std::filesystem::path& destination)>
        before_move;
    // Called before a restore removes pending.json or journal.json; throwing makes it fail.
    std::function<void(const std::filesystem::path& path)> before_remove;
};

// The installed faults (none by default).
RestoreFaults& restore_faults();

// Installs `faults` for its lifetime.
class ScopedRestoreFaults {
public:
    explicit ScopedRestoreFaults(RestoreFaults faults);
    ~ScopedRestoreFaults();
    ScopedRestoreFaults(const ScopedRestoreFaults&) = delete;
    ScopedRestoreFaults& operator=(const ScopedRestoreFaults&) = delete;
};

// Thrown by a hook to model the process dying at that point: the restore unwinds without
// rolling back or removing its journal, as a killed process leaves them.
class SimulatedCrash : public std::runtime_error {
public:
    SimulatedCrash() : std::runtime_error("simulated crash") {}
};

} // namespace fv::backup::testing
