#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include "base/env.h"
#include "base/identity.h"

namespace fv {

// Where OpenWhisper_CPP keeps its files (architecture 7.1). A source checkout never becomes the
// data root: OpenWhisper's checkout rule exists for `python main.py` (a deliberate difference).
// Resolved once, where OpenWhisper's config.py re-evaluates data_root() on every call.
// Affinity: thread-safe (immutable once resolved).
class Paths {
public:
    // <env_prefix>DATA_DIR, when set and not empty, is expanded ("~"), made absolute and used;
    // otherwise the per-user directory. The data root is created (std::filesystem::filesystem_error
    // when it cannot be).
    static Paths resolve(const AppIdentity&, const EnvReader&); // OPENWHISPER_CPP_DATA_DIR wins
    const std::filesystem::path& data_root() const;             // %LOCALAPPDATA%\OpenWhisper_CPP
    // %LOCALAPPDATA%\<data_dir_name> (the home folder when LOCALAPPDATA is unset or empty);
    // $XDG_DATA_HOME or ~/.local/share on Linux; ~/Library/Application Support on macOS.
    // Not moved by <env_prefix>DATA_DIR.
    std::filesystem::path local_app_dir() const;
    std::filesystem::path components_root() const;                // <local_app_dir>/components
    std::filesystem::path speech_models_root() const;             // <local_app_dir>/speech-models
    std::filesystem::path user_file(std::string_view name) const; // data_root()/name
    // <data_root>/.env, whether or not it exists (OpenWhisper's checkout fallback is dropped).
    std::filesystem::path env_file() const;
    // OpenWhisper's data roots, for import (architecture 7.5) and adoption (7.6), not checked
    // for existence: OPENWHISPER_DATA_DIR (expanded, absolute) when set and not empty, then
    // OpenWhisper's per-user directory, which also holds its components\ and speech-models\.
    std::vector<std::filesystem::path> openwhisper_roots() const; // import and adoption sources

private:
    Paths() = default;

    std::filesystem::path data_root_;
    std::filesystem::path local_app_dir_;
    std::vector<std::filesystem::path> openwhisper_roots_;
};

// os.path.expanduser over `env` with Python 3.12's rules (USERPROFILE, else HOMEDRIVE and
// HOMEPATH, and a "~" before a backslash on Windows; HOME with its trailing separators dropped,
// else the password database, and only "~/" elsewhere); the path unchanged when no home can be
// found.
std::filesystem::path expand_user(const std::filesystem::path& path, const EnvReader& env);

} // namespace fv
