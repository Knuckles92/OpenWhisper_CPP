#include "backup/members.h"

#include <algorithm>
#include <optional>
#include <system_error>

#include "backup/backup.h"
#include "backup/detail/fs.h"
#include "base/utf8.h"

namespace fv::backup {

namespace {

constexpr std::string_view kUnsafe = "Unsafe archive member name";

std::vector<std::string> split(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = text.find('/', start);
        parts.emplace_back(text.substr(start, slash - start));
        if (slash == std::string_view::npos) {
            return parts;
        }
        start = slash + 1;
    }
}

struct Listing {
    std::vector<std::string> dirs;
    std::vector<std::string> files;
};

// One step of os.walk(top, followlinks=False): the entries of `folder` split as scandir's
// is_dir() splits them (links followed). A folder that cannot be listed yields nothing, as
// os.walk skips it without onerror.
std::optional<Listing> list_folder(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::directory_iterator it(folder, error);
    if (error) {
        return std::nullopt;
    }
    Listing listing;
    for (const std::filesystem::directory_iterator end; it != end; it.increment(error)) {
        if (error) {
            break;
        }
        std::error_code type_error;
        const bool is_dir = it->is_directory(type_error);
        std::string name = path_to_utf8(it->path().filename());
        (is_dir && !type_error ? listing.dirs : listing.files).push_back(std::move(name));
    }
    std::ranges::sort(listing.dirs);
    std::ranges::sort(listing.files);
    return listing;
}

void walk(const std::filesystem::path& folder, const std::string& relative, const std::string& name,
          bool at_base, const Names& names,
          std::vector<std::pair<std::string, std::filesystem::path>>& files) {
    std::optional<Listing> listing = list_folder(folder);
    if (!listing) {
        return;
    }
    std::erase_if(listing->dirs, [&](const std::string& directory) {
        return name == kRemoteRecords && at_base && directory == "cache";
    });
    for (const auto& directory : listing->dirs) {
        if (detail::is_link(folder / path_from_utf8(directory))) {
            throw BackupError("Symbolic links are not supported in backed-up data");
        }
    }
    for (const auto& filename : listing->files) {
        const std::filesystem::path path = folder / path_from_utf8(filename);
        if (detail::is_link(path) || !detail::stat(path).regular) {
            throw BackupError("Symbolic links and special files cannot be backed up");
        }
        const std::string member = "data/" + relative + "/" + filename;
        safe_member(member, names);
        files.emplace_back(member, path);
    }
    for (const auto& directory : listing->dirs) {
        walk(folder / path_from_utf8(directory), relative + "/" + directory, name, false, names,
             files);
    }
}

} // namespace

std::vector<std::string> member_parts(std::string_view name) {
    if (name.empty() || name.find('\\') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos) {
        throw BackupError(std::string(kUnsafe));
    }
    if (name.starts_with('/') ||
        std::ranges::any_of(name, [](char c) { return static_cast<unsigned char>(c) < 32; })) {
        throw BackupError(std::string(kUnsafe));
    }
    std::vector<std::string> parts = split(name);
    if (std::ranges::any_of(parts, [](const std::string& part) {
            return part.empty() || part == "." || part == ".." ||
                   part.find(':') != std::string::npos;
        })) {
        throw BackupError(std::string(kUnsafe));
    }
    // str(PurePosixPath(name)) != name: with the checks above every part is plain, so the
    // normal form is the parts joined again.
    std::string canonical;
    for (const auto& part : parts) {
        canonical += (canonical.empty() ? "" : "/") + part;
    }
    if (canonical != name) {
        throw BackupError("Noncanonical archive member name");
    }
    return parts;
}

std::vector<std::string> member_parts(const Json& name) {
    if (!name.is_string()) {
        throw BackupError(std::string(kUnsafe));
    }
    return member_parts(name.get_ref<const std::string&>());
}

std::string safe_member(std::string_view name, const Names& names) {
    const std::vector<std::string> parts = member_parts(name);
    if (name == kManifestName) {
        return std::string(name);
    }
    check_owned_member(parts, names);
    return std::string(name);
}

std::string safe_member(const Json& name, const Names& names) {
    if (!name.is_string()) {
        throw BackupError(std::string(kUnsafe));
    }
    return safe_member(std::string_view(name.get_ref<const std::string&>()), names);
}

void check_owned_member(const std::vector<std::string>& parts, const Names& names) {
    if (parts.size() < 2 || parts[0] != "data" || !names.is_owned(parts[1])) {
        throw BackupError("Archive contains a file outside owned data");
    }
    if (is_legacy_history_name(parts[1]) || parts[1] == kRemoteRecords) {
        throw BackupError("Archive contains transient or raw legacy data");
    }
    if (parts.size() > 2 && !is_recording_name(parts[1])) {
        throw BackupError("Archive contains an unexpected nested file");
    }
    if (parts[1] == kRemoteRecords && parts.size() > 2 && parts[2] == "cache") {
        throw BackupError("Archive contains remote cache");
    }
    if (parts[1] == names.restore_dir) {
        throw BackupError("Archive contains internal restore data");
    }
}

std::filesystem::path safe_path(const std::filesystem::path& root, std::string_view name,
                                const Names& names) {
    // SQLite sidecars are moved during restore but never exported; the online backup has
    // already folded committed WAL pages into the snapshot.
    if (!names.is_sqlite_sidecar(name) && !is_legacy_history_name(name) && name != kRemoteRecords) {
        safe_member("data/" + std::string(name), names);
    }
    const std::filesystem::path path = detail::join_parts(root, name);
    if (!detail::inside(path, root)) {
        throw BackupError("Path escapes the data directory");
    }
    std::filesystem::path cursor = root;
    for (const auto& part : split(name)) {
        cursor /= path_from_utf8(part);
        if (detail::is_link(cursor)) {
            throw BackupError("Symbolic links are not supported in backed-up data: " +
                              std::string(name));
        }
    }
    return path;
}

bool archived_separately(std::string_view name, bool include_recordings, const Names& names) {
    if (name == names.database_file || name == names.settings_file) {
        return true; // snapshot and sanitized copy are added separately
    }
    if (is_legacy_history_name(name)) {
        return true; // live legacy entries are merged into the database snapshot
    }
    if (name == kRemoteRecords) {
        return true; // staging, exports and cache are transient sync data
    }
    return !include_recordings && is_recording_name(name);
}

std::vector<std::pair<std::string, std::filesystem::path>>
owned_files(const std::filesystem::path& root, const Names& names, bool include_recordings) {
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (const auto& name : names.owned) {
        if (archived_separately(name, include_recordings, names)) {
            continue;
        }
        const std::filesystem::path base = safe_path(root, name, names);
        if (detail::is_link(base)) {
            throw BackupError("Symbolic links are not supported in backed-up data: " + name);
        }
        if (!detail::exists(base)) {
            continue;
        }
        if (detail::is_file(base)) {
            if (!detail::stat(base).regular) {
                throw BackupError("Unsupported data file: " + name);
            }
            files.emplace_back("data/" + name, base);
            continue;
        }
        if (!detail::is_dir(base)) {
            throw BackupError("Unsupported data path: " + name);
        }
        walk(base, name, name, true, names, files);
    }
    return files;
}

} // namespace fv::backup
