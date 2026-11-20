#pragma once

#include <filesystem>

#include "base/json.h"

// What a bundle (and a restore, and an import) does to the settings it carries
// (services/backup.py:248-360): pairings, the knowledge-folder path and secret-named keys go,
// network and automation opt-ins are turned off, saved hooks and active actions come back
// disabled, and endpoint URLs lose their credentials, query and fragment.
namespace fv::backup {

// _scrub: dict keys holding a secret word ("password", "secret", "private_key", "api_key",
// "credential", "token", "authorization", "header", "cookie"; any case) are dropped at any
// depth, and an http(s) URL with a host is cut to scheme, host and path. Below
// "dictation_snippets" (the user's own text) URLs are kept. A string urlsplit refuses becomes ""
// when it starts with http:// or https://.
Json scrub(const Json& value, bool keep_urls = false);

// _sanitize_actions: the saved actions as the app's loader reads them (rows it drops are
// dropped), scrubbed, with a link action's template kept whole and webhook, program and agent
// actions disabled.
Json sanitize_actions(const Json& raw);

// _sanitize_settings: `settings` must be an object.
Json sanitize_settings(const Json& settings);

// _read_settings: {} when the file does not exist; BackupError when it is a link or not a file
// ("Settings path is unsafe"), cannot be read or parsed ("Settings JSON cannot be read"), or is
// not an object ("Settings JSON must be an object").
Json read_settings(const std::filesystem::path& path);

} // namespace fv::backup
