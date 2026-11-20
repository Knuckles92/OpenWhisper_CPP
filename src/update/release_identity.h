#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The names a FioriVoce release is published and installed under: the Inno AppId, the
// executables, the setup asset and where updates come from. One file,
// src/update/data/release_identity.json, holds them for the app, the installer and the release
// tools alike; the AppId can never change once a setup has been installed.
namespace fv::update {

// Where release metadata comes from. None until FioriVoce has a public repository: such a build
// never checks on its own, and only the loopback feed (FIORIVOCE_UPDATE_FEED_URL) can offer it
// an update.
struct UpdateSource {
    enum class Kind { None, GitHub } kind = Kind::None;
    std::string repo; // GitHub: "<owner>/<name>"
};

// Affinity: immutable once read.
struct ReleaseIdentity {
    std::string app_id; // the Inno AppId, braced upper-case GUID: "{XXXXXXXX-XXXX-...}"
    std::string publisher, publisher_url, install_dir_name, app_exe, cli_exe, setup_prefix;
    UpdateSource source;
    std::string timestamp_url; // Authenticode timestamp server of the signing step

    std::string app_id_bare() const;                 // the GUID without braces
    std::string app_mutex() const;                   // "FioriVoce-App-<bare guid>"
    std::string app_mutex_global() const;            // "Global\\FioriVoce-App-<bare guid>"
    std::string setup_mutex() const;                 // "Global\\FioriVoce-Setup-<bare guid>"
    std::vector<std::string> app_mutexes() const;    // app_mutex(), app_mutex_global()
    std::vector<std::string> update_mutexes() const; // "FioriVoce-Update-<bare>", its Global\ twin
    std::string inno_uninstall_key() const;          // "<app_id>_is1"
    std::string uninstall_key_path() const;          // under HKCU or HKLM
};

// The identity embedded from data/release_identity.json, read once.
const ReleaseIdentity& release_identity();

// Reads one identity document. Throws std::invalid_argument naming the key that is missing or
// malformed (app_id must be a braced upper-case GUID; source.kind "none" or "github" with an
// "<owner>/<name>" repo).
ReleaseIdentity parse_release_identity(std::string_view json_text);

// What an update check talks to: the identity's source, the loopback feed that replaces it when
// FIORIVOCE_UPDATE_FEED_URL is valid (update/release_feed.h feed_config), and the User-Agent
// ("" sends http::user_agent()).
struct FeedConfig {
    UpdateSource source;
    std::optional<std::string> override_url;
    std::string user_agent;
};

} // namespace fv::update
