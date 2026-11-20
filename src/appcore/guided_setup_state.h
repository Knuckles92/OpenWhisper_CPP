#pragma once

#include <array>
#include <filesystem>
#include <string_view>

#include "base/identity.h"

namespace fv {
class SettingsStore;
}

// Conservative, durable eligibility for the optional first-run guided setup
// (services/guided_setup.py). Only a launch that started without settings and without prior
// user data is invited, and a saved pending invitation survives other first-launch writes.
// Call initialize before the main window, the database or History can create user-data files.
// Affinity: thread-safe; blocking (reads the file system), never on the GUI thread.
namespace fv::appcore::guided_setup_state {

// The values of guided_setup_state; no others are saved.
inline constexpr std::string_view kPending = "pending";
inline constexpr std::string_view kStarted = "started";
inline constexpr std::string_view kSkipped = "skipped";
inline constexpr std::string_view kCompleted = "completed";
inline constexpr std::string_view kExisting = "existing";
inline constexpr std::array<std::string_view, 5> kStates{kPending, kStarted, kSkipped, kCompleted,
                                                         kExisting};

// _has_prior_data: whether `root` (the settings file's folder) holds the database,
// transcription_history.json or scratchpad.txt (existence only, even empty), or a non-empty
// recordings/ or meetings/ folder. Settings may have been deleted or moved; users with history,
// recordings, meetings or an autosaved scratchpad are not interrupted.
bool has_prior_data(const std::filesystem::path& root, const AppIdentity& id = identity());

// Seeds only guided_setup_state, keeping every other preference: nothing when it is saved
// already; otherwise "pending" when this launch started without the settings file and its
// folder holds no prior data, else "existing", written as setdefault under the store's lock.
// Any failure (corrupt or unreadable settings) is logged and leaves the state unset: such a
// file is neither overwritten nor invited.
void initialize(SettingsStore& store, const AppIdentity& id = identity());

bool should_invite(const SettingsStore& store);
bool is_completed(const SettingsStore& store);

// Saves `state`; py::ValueError "Unknown guided setup state: <state>" for any other value.
void save_state(std::string_view state, SettingsStore& store);

} // namespace fv::appcore::guided_setup_state
