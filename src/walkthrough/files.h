#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "base/json.h"

// The file rules every walkthrough folder keeps (services/walkthrough/library.py:26-99): only
// local, regular files directly inside a capture are read, metadata is replaced atomically, and
// a set of rendered files is published together or not at all.
namespace fv::walkthrough {

inline constexpr std::string_view DRAFT_NAME = "draft.json";
inline constexpr std::string_view AUDIO_NAME = "audio.wav";
// captures/retention.json.
inline constexpr std::string_view PREFERENCES_NAME = "retention.json";
// The largest metadata file read_json accepts.
inline constexpr std::uintmax_t MAX_METADATA_BYTES = 8 * 1024 * 1024;

// Replaces `path` with `value` as json.dump(ensure_ascii=False, indent=2, allow_nan=False)
// writes it: LF line endings, no final newline, flushed to disk before it replaces the old file.
// A NaN or infinity is fv::py::ValueError("Out of range float values are not JSON compliant:
// nan") and nothing is written; a failed write is std::filesystem::filesystem_error, with no
// temporary file left behind and the old file untouched.
void atomic_json(const std::filesystem::path& path, const Json& value);

// Moves stage/<name> over folder/<name> for every name, after deleting folder/<name> for every
// name in `remove`. The files it replaces or deletes are first copied to stage/.previous (which
// must not exist yet); when a replacement fails, everything done so far is undone in reverse
// order from there and the error is rethrown. A target that is a link is refused before anything
// changes (local_file's fv::py::ValueError).
void publish_files(const std::filesystem::path& stage, const std::filesystem::path& folder,
                   std::span<const std::string> names, std::span<const std::string> remove = {});

// folder/<name> when it is a regular file directly inside a folder that is not a link.
// fv::py::ValueError "Invalid walkthrough filename" for a name with a separator (or one a
// Windows path reads as a drive), "Missing or unsafe walkthrough file: <name>" otherwise.
std::filesystem::path local_file(const std::filesystem::path& folder, std::string_view name);

// The JSON object in `path`, which may not be a link nor larger than MAX_METADATA_BYTES
// (fv::py::ValueError "Unsafe or oversized walkthrough metadata") and must hold an object
// ("Walkthrough metadata must be an object"). Text that is not UTF-8 or not JSON is
// fv::py::ValueError (fv::py::JsonError for the latter); a missing file is
// std::filesystem::filesystem_error.
Json read_json(const std::filesystem::path& path);

} // namespace fv::walkthrough
