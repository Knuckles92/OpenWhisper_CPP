#include "backup/names.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

namespace fv::backup {

namespace {

template <class Container> bool contains(const Container& names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

Names make_names(const AppIdentity& identity) {
    Names names;
    names.identity = &identity;
    names.format = identity.backup_format;
    names.extension = identity.backup_extension;
    names.settings_file = identity.settings_file;
    names.database_file = identity.database_file;
    names.restore_dir = identity.restore_dir;
    names.owned = {
        names.settings_file,
        names.database_file,
        "transcription_history.json",
        "transcription_history.json.bak",
        "meetings.json",
        "meetings.json.bak",
        "recorded_audio.wav",
        ".recorded_audio-recovery",
        "recordings",
        "meetings",
        "captures",
        "remote_records",
        "scratchpad.txt",
    };
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        names.sqlite_sidecars.push_back(names.database_file + suffix);
    }
    names.restore_names = names.owned;
    names.restore_names.insert(names.restore_names.end(), names.sqlite_sidecars.begin(),
                               names.sqlite_sidecars.end());
    names.settings_member = "data/" + names.settings_file;
    names.database_member = "data/" + names.database_file;
    return names;
}

} // namespace

bool is_recording_name(std::string_view name) {
    return contains(kRecordingNames, name);
}

bool is_legacy_history_name(std::string_view name) {
    return contains(kLegacyHistoryNames, name);
}

bool Names::is_owned(std::string_view name) const {
    return contains(owned, name);
}

bool Names::is_sqlite_sidecar(std::string_view name) const {
    return contains(sqlite_sidecars, name);
}

const Names& names_for(const AppIdentity& identity) {
    static std::mutex mutex;
    static std::map<const AppIdentity*, std::unique_ptr<Names>> cache;
    const std::lock_guard lock(mutex);
    auto& slot = cache[&identity];
    if (!slot) {
        slot = std::make_unique<Names>(make_names(identity));
    }
    return *slot;
}

} // namespace fv::backup
