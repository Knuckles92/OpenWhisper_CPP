#include "appcore/guided_setup_state.h"

#include <algorithm>
#include <exception>
#include <format>
#include <string>
#include <system_error>

#include "base/log.h"
#include "base/utf8.h"
#include "pyport/errors.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::appcore::guided_setup_state {

namespace {

constexpr log::Logger kLog{"services.guided_setup"};

// Path.exists(): false for a path that is not there, but a path that cannot be checked
// (permission denied, an unreachable share) raises, which initialize catches, so data it could
// not see never reads as a new install.
bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    const bool found = std::filesystem::exists(path, error);
    if (error) {
        throw std::filesystem::filesystem_error("Could not check for prior data", path, error);
    }
    return found;
}

bool has_entries(const std::filesystem::path& folder) {
    if (!path_exists(folder)) {
        return false;
    }
    // next(folder.iterdir(), None): an unreadable folder raises in Python, which initialize
    // catches; the same error is thrown here.
    const std::filesystem::directory_iterator it(folder);
    return it != std::filesystem::directory_iterator();
}

} // namespace

bool has_prior_data(const std::filesystem::path& root, const AppIdentity& id) {
    for (const std::string_view name :
         {id.database_file, std::string_view("transcription_history.json"),
          std::string_view("scratchpad.txt")}) {
        if (path_exists(root / path_from_utf8(name))) {
            return true;
        }
    }
    for (const char* name : {"recordings", "meetings"}) {
        if (has_entries(root / name)) {
            return true;
        }
    }
    return false;
}

void initialize(SettingsStore& store, const AppIdentity& id) {
    const std::string key(keys::guided_setup_state.key());
    try {
        const auto current = store.snapshot();
        if (current->is_object() && current->contains(key)) {
            return;
        }
        std::string_view state = kExisting;
        if (store.is_new_install() && !has_prior_data(store.path().parent_path(), id)) {
            state = kPending;
        }
        // Rechecked under the store's lock.
        store.mutate([&](Json& settings) {
            if (!settings.contains(key)) {
                settings[key] = std::string(state);
            }
        });
    } catch (const std::exception& error) {
        // Corrupt or unreadable settings and ambiguous data must not look new.
        kLog.error_with(error, "Could not initialize guided setup eligibility");
    }
}

bool should_invite(const SettingsStore& store) {
    return keys::guided_setup_state.read(*store.snapshot()) == kPending;
}

bool is_completed(const SettingsStore& store) {
    return keys::guided_setup_state.read(*store.snapshot()) == kCompleted;
}

void save_state(std::string_view state, SettingsStore& store) {
    if (std::ranges::find(kStates, state) == kStates.end()) {
        throw py::ValueError(std::format("Unknown guided setup state: {}", state));
    }
    store.save_setting(keys::guided_setup_state.key(), Json(std::string(state)));
}

} // namespace fv::appcore::guided_setup_state
