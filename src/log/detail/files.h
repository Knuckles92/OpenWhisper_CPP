#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"

// File and platform helpers shared by the log sinks and the diagnostics files.
namespace fv::log::detail {

// What Python's text mode writes for `text`: "\n" becomes "\r\n" on Windows (os.linesep).
std::string text_mode_bytes(std::string_view text);

// Python's Path.write_text to `path`'s with_suffix(".tmp") sibling, then os.replace onto `path`,
// creating the folder first. Throws std::filesystem::filesystem_error.
void write_text_replace(const std::filesystem::path& path, std::string_view text);

// Python's Path.with_suffix: the last suffix of the file name replaced (or added).
std::filesystem::path with_suffix(const std::filesystem::path& path, std::string_view suffix);

// Python's open(path, "x") and write: fails with std::filesystem::filesystem_error
// (errc::file_exists) when the path exists, and then leaves it untouched.
void write_new_file(const std::filesystem::path& path, std::string_view bytes);

// diagnostics._read_json: JSON null for a file over 1 MiB, a missing or unreadable file, text
// that is not UTF-8, or text json.loads refuses.
Json read_json_file(const std::filesystem::path& path);

// Strict UTF-8, as Python's "utf-8" codec decodes it (no surrogates, no overlong forms).
bool valid_utf8(std::string_view bytes);

// datetime.now(timezone.utc).isoformat() for a given instant: microseconds only when non-zero.
std::string utc_isoformat(std::chrono::system_clock::time_point time);

// Names the calling thread for debuggers and crash dumps.
void set_thread_name(const char* name);

// The process's standard error: UTF-16 to a console on Windows, the bytes otherwise.
void write_stderr(std::string_view utf8);

// platform.system(), platform.release() and os.cpu_count().
std::string os_system();
std::string os_release();
std::optional<std::int64_t> cpu_count();

} // namespace fv::log::detail
