#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backup/names.h"
#include "base/json.h"

// services/backup.py's member and path rules (106-245). Every refusal is a BackupError.
namespace fv::backup {

// _member_parts: a canonical, relative POSIX member name split into its parts. Refuses ("Unsafe
// archive member name") an empty name, "\", NUL or a control character, a leading "/", an empty,
// "." or ".." part and a part with ":"; "Noncanonical archive member name" otherwise when the
// name is not its own POSIX normal form.
std::vector<std::string> member_parts(std::string_view name);
// A manifest's "path" field: anything but a string is "Unsafe archive member name".
std::vector<std::string> member_parts(const Json& name);
inline std::vector<std::string> member_parts(const std::string& name) {
    return member_parts(std::string_view(name));
}
inline std::vector<std::string> member_parts(const char* name) {
    return member_parts(std::string_view(name));
}

// _safe_member: "manifest.json", or a member under data/ that check_owned_member accepts.
std::string safe_member(std::string_view name, const Names& names);
std::string safe_member(const Json& name, const Names& names);
// Text would otherwise be ambiguous between the two overloads above.
inline std::string safe_member(const std::string& name, const Names& names) {
    return safe_member(std::string_view(name), names);
}
inline std::string safe_member(const char* name, const Names& names) {
    return safe_member(std::string_view(name), names);
}

// _check_owned_member: data/<owned name>, nested only below a recording folder, never the
// legacy history files, remote_records or the restore folder.
void check_owned_member(const std::vector<std::string>& parts, const Names& names);

// _safe_path: root joined with the "/"-separated name, which must stay inside root and pass no
// symbolic link or junction ("Symbolic links are not supported in backed-up data: <name>").
// Sidecars, legacy history names and remote_records skip the member check.
std::filesystem::path safe_path(const std::filesystem::path& root, std::string_view name,
                                const Names& names);

// _archived_separately: owned names whose live files are not copied as they are (the database
// snapshot, the sanitized settings, the legacy history merged into the snapshot, remote_records,
// and the recordings of a text-only bundle).
bool archived_separately(std::string_view name, bool include_recordings, const Names& names);

// _owned_files: (member name, file) for every owned file that goes into the bundle as it is, in
// OWNED_NAMES order, folders walked top-down with their files and subfolders sorted. Links and
// special files are refused.
std::vector<std::pair<std::string, std::filesystem::path>>
owned_files(const std::filesystem::path& root, const Names& names, bool include_recordings);

} // namespace fv::backup
