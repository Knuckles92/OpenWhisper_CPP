#pragma once

#include <string>
#include <string_view>

#include "settings/setting.h"

// open(path, encoding="utf-8").read() followed by json.load, as SettingsManager reads the file.
namespace fv::detail {

// The UTF-8 decode of the whole file: throws SettingsFileError("UnicodeDecodeError", ...) with
// CPython's message ("'utf-8' codec can't decode byte 0xff in position 7: invalid start byte"),
// positions in bytes of the file.
void check_utf8(std::string_view bytes);

// Text mode's universal newlines: "\r\n" and a lone "\r" read as "\n".
std::string universal_newlines(std::string_view text);

// The settings mapping in the file's bytes. Throws SettingsFileError carrying the Python
// exception json.load or the root check raised.
Json parse_settings(std::string_view bytes);

// The bytes SettingsManager writes for `settings`: json.dump(settings, handle, indent=2) and a
// newline, through a text-mode handle, so every "\n" becomes os.linesep ("\r\n" on Windows).
// ensure_ascii escapes every control character inside strings, so the only newlines are the
// layout's.
std::string settings_text(const Json& settings);

} // namespace fv::detail
