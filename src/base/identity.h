#pragma once

#include <string_view>

namespace fv {

// Every name that differs between FioriVoce and OpenWhisper. Nothing else in the tree spells
// either product name, except remote/wire.h (protocol constants) and user-visible copy.
// Affinity: thread-safe (immutable).
struct AppIdentity {
    std::string_view product;             // "FioriVoce"
    std::string_view data_dir_name;       // "FioriVoce"
    std::string_view env_prefix;          // "FIORIVOCE_"  (FIORIVOCE_DATA_DIR, ..._LOG_LEVEL)
    std::string_view settings_file;       // "fiorivoce_settings.json"
    std::string_view database_file;       // "fiorivoce.db"
    std::string_view log_file;            // "fiorivoce.log"
    std::string_view lease_file;          // ".fiorivoce-data.lock"
    std::string_view restore_dir;         // ".fiorivoce-restore"
    std::string_view credential_service;  // "FioriVoce"
    std::string_view backup_format;       // "fiorivoce-backup"
    std::string_view backup_extension;    // ".fvbackup"
    std::string_view instance_key_prefix; // "fiorivoce-ui"
    // The product name as people say it to a meeting's note taker, lower case, as one word and
    // as two. Shown as `product`.
    std::string_view wake_name;        // "fiorivoce"
    std::string_view wake_name_spaced; // "fiori voce"
};

const AppIdentity& identity();        // FioriVoce
const AppIdentity& legacy_identity(); // OpenWhisper: import and .owbackup reading only

// Values OpenWhisper stores in its settings file. The file's format is shared and imported, so
// FioriVoce reads and writes them unchanged.
// The agent picker's built-in tile (agent_picker.py BUILTIN).
extern const std::string_view legacy_builtin_agent_id;

} // namespace fv
