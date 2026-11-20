#include "base/identity.h"

namespace fv {

const AppIdentity& identity() {
    static constexpr AppIdentity value{
        .product = "FioriVoce",
        .data_dir_name = "FioriVoce",
        .env_prefix = "FIORIVOCE_",
        .settings_file = "fiorivoce_settings.json",
        .database_file = "fiorivoce.db",
        .log_file = "fiorivoce.log",
        .lease_file = ".fiorivoce-data.lock",
        .restore_dir = ".fiorivoce-restore",
        .credential_service = "FioriVoce",
        .backup_format = "fiorivoce-backup",
        .backup_extension = ".fvbackup",
        .instance_key_prefix = "fiorivoce-ui",
        .wake_name = "fiorivoce",
        .wake_name_spaced = "fiori voce",
    };
    return value;
}

// OpenWhisper's names (config.py APP_NAME, DATA_DIR_ENV, SETTINGS_FILENAME; backup_startup.py
// LOCK_FILENAME; backup.py's format and extension), for import and .owbackup reading only.
const AppIdentity& legacy_identity() {
    static constexpr AppIdentity value{
        .product = "OpenWhisper",
        .data_dir_name = "OpenWhisper",
        .env_prefix = "OPENWHISPER_",
        .settings_file = "openwhisper_settings.json",
        .database_file = "openwhisper.db",
        .log_file = "openwhisper.log",
        .lease_file = ".openwhisper-data.lock",
        .restore_dir = ".openwhisper-restore",
        .credential_service = "OpenWhisper",
        .backup_format = "openwhisper-backup",
        .backup_extension = ".owbackup",
        .instance_key_prefix = "openwhisper-ui",
        // settings.py DEFAULT_VOICE_COMMAND_NAMES: a settings file imported from OpenWhisper may
        // store them, and they are shown as OpenWhisper's product name (voice_help.py).
        .wake_name = "openwhisper",
        .wake_name_spaced = "open whisper",
    };
    return value;
}

const std::string_view legacy_builtin_agent_id = "openwhisper";

} // namespace fv
