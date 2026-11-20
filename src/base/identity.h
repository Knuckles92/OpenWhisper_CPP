#pragma once

#include <string_view>

namespace fv {

// Every name that differs between OpenWhisper_CPP and OpenWhisper. Nothing else in the tree spells
// either product name, except remote/wire.h (protocol constants) and user-visible copy.
// Affinity: thread-safe (immutable).
struct AppIdentity {
    std::string_view product;       // "OpenWhisper_CPP"
    std::string_view data_dir_name; // "OpenWhisper_CPP"
    std::string_view env_prefix;    // "OPENWHISPER_CPP_"  (OPENWHISPER_CPP_DATA_DIR, ..._LOG_LEVEL)
    std::string_view settings_file; // "openwhisper_cpp_settings.json"
    std::string_view database_file; // "openwhisper-cpp.db"
    std::string_view log_file;      // "openwhisper-cpp.log"
    std::string_view lease_file;    // ".openwhisper-cpp-data.lock"
    std::string_view restore_dir;   // ".openwhisper-cpp-restore"
    std::string_view credential_service;  // "OpenWhisper_CPP"
    std::string_view backup_format;       // "openwhisper-cpp-backup"
    std::string_view backup_extension;    // ".fvbackup"
    std::string_view instance_key_prefix; // "openwhisper-cpp-ui"
    // The product name as people say it to a meeting's note taker, in compact and
    // spaced forms. Shown as `product`.
    std::string_view wake_name;        // "openwhispercpp"
    std::string_view wake_name_spaced; // "open whisper c p p"
};

const AppIdentity& identity();        // OpenWhisper_CPP
const AppIdentity& legacy_identity(); // OpenWhisper: import and .owbackup reading only

// Values OpenWhisper stores in its settings file. The file's format is shared and imported, so
// OpenWhisper_CPP reads and writes them unchanged.
// The agent picker's built-in tile (agent_picker.py BUILTIN).
extern const std::string_view legacy_builtin_agent_id;

} // namespace fv
