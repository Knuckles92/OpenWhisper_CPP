#include "meeting_state/data_lifecycle.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "base/ids.h"
#include "base/log.h"
#include "base/utf8.h"
#include "meeting_state/detail/py_value.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.persist.data_lifecycle");

namespace fs = std::filesystem;
using detail::get;
using detail::truthy;

std::string text_field(const Json& row, std::string_view key) {
    const Json& value = get(row, key);
    return truthy(value) ? detail::py_str(value) : std::string();
}

// os.path.realpath(os.path.abspath(path)), without a trailing separator. realpath is not strict:
// a component it cannot resolve (access denied, a dangling link) is kept as written, never an
// error that would stop a clear halfway.
fs::path real_path(const fs::path& path) {
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error).lexically_normal();
    fs::path real = fs::weakly_canonical(absolute, error);
    if (error) {
        real = absolute;
    }
    if (!real.has_filename() && real.has_relative_path()) {
        real = real.parent_path();
    }
    return real;
}

// _validated_spool_path: a direct child of the meetings root, so a corrupt or hostile row can
// never point a delete at the root, a parent or a sibling tree.
std::optional<fs::path> validated_spool_path(const std::string& spool_dir,
                                             const fs::path& meetings_root) {
    if (spool_dir.empty() || meetings_root.empty()) {
        return std::nullopt;
    }
    const fs::path root = real_path(meetings_root);
    const fs::path spool = real_path(path_from_utf8(spool_dir));
    if (spool == root || spool.parent_path() != root) {
        throw py::ValueError("meeting spool is outside the configured meetings root");
    }
    return spool;
}

// _restore_spool: the tombstone goes back unless something already took the spool's place.
void restore_spool(const fs::path& tombstone, const fs::path& spool, std::string_view failure) {
    std::error_code error;
    if (!fs::is_directory(tombstone, error) || fs::exists(spool, error)) {
        return;
    }
    fs::rename(tombstone, spool, error);
    if (error) {
        logger.error("{}: {}", failure, error.message());
    }
}

// _tombstone_spool: renamed aside, so the database delete is the commit point.
fs::path tombstone_spool(const fs::path& spool, const std::string& meeting_id) {
    const fs::path tombstone = spool.parent_path() / path_from_utf8(std::string(TOMBSTONE_PREFIX) +
                                                                    meeting_id + "-" + uuid4_hex());
    try {
        fs::rename(spool, tombstone);
        // A rename keeps the folder's old mtime. Refreshed, a purge measures the age of this
        // deletion, not the age of the meeting's last audio chunk.
        fs::last_write_time(tombstone, fs::file_time_type::clock::now());
    } catch (const std::exception&) {
        restore_spool(tombstone, spool,
                      "Could not restore meeting spool after tombstone setup failed: " +
                          meeting_id);
        throw;
    }
    return tombstone;
}

void remove_tree(const fs::path& path, std::string_view failure) {
    std::error_code error;
    if (fs::is_symlink(path, error)) {
        logger.error("{}: Cannot call rmtree on a symbolic link", failure);
        return;
    }
    if (!fs::is_directory(path, error)) {
        logger.error("{}: not a directory", failure);
        return;
    }
    fs::remove_all(path, error);
    if (error) {
        logger.error("{}: {}", failure, error.message());
    }
}

void deleted(const Json& meeting, const MeetingDeleted& on_deleted) {
    if (on_deleted) {
        on_deleted(text_field(meeting, "id"), truthy(get(meeting, "origin_device_id")));
    }
}

void purge_stale_tombstone(const fs::path& path) {
    std::error_code error;
    const auto modified = fs::last_write_time(path, error);
    if (error) {
        return;
    }
    const auto age = fs::file_time_type::clock::now() - modified;
    const double age_s =
        std::max(0.0, std::chrono::duration_cast<std::chrono::duration<double>>(age).count());
    if (age_s < DELETION_TOMBSTONE_STALE_S) {
        return;
    }
    remove_tree(path, "Could not purge abandoned deletion tombstone " + path_to_utf8(path));
}

// A direct-child spool directory of the root that no kept meeting owns.
void purge_unkept_spool(const fs::path& path, const fs::path& root,
                        const std::set<fs::path>& keep) {
    std::error_code error;
    if (!fs::is_directory(path, error)) {
        return;
    }
    const fs::path real = real_path(path);
    if (real == root || real.parent_path() != root || keep.contains(real)) {
        return;
    }
    remove_tree(real, "Could not purge leftover meeting spool " + path_to_utf8(real));
}

// _purge_orphan_spools. Concurrent single-meeting deletes rename a spool to the tombstone
// prefix before their commit: recent tombstones stay, but a crash can strand one forever, so
// stale ones are collected.
void purge_orphan_spools(const fs::path& meetings_root, const std::set<std::string>& keep_spools) {
    std::error_code error;
    if (meetings_root.empty() || !fs::is_directory(meetings_root, error)) {
        return;
    }
    const fs::path root = real_path(meetings_root);
    std::set<fs::path> keep;
    for (const auto& spool : keep_spools) {
        if (!spool.empty()) {
            keep.insert(real_path(path_from_utf8(spool)));
        }
    }
    std::vector<fs::path> names;
    try {
        for (const auto& entry : fs::directory_iterator(root)) {
            names.push_back(entry.path().filename());
        }
    } catch (const std::exception& failure) {
        logger.error_with(failure, "Could not list meeting spools in " + path_to_utf8(root));
        return;
    }
    for (const auto& name : names) {
        const fs::path path = root / name;
        if (path_to_utf8(name).starts_with(TOMBSTONE_PREFIX)) {
            purge_stale_tombstone(path);
        } else {
            purge_unkept_spool(path, root, keep);
        }
    }
}

} // namespace

bool delete_meeting_data(MeetingRepository& repository, const std::string& meeting_id,
                         const fs::path& meetings_root, bool delete_spool,
                         MeetingDeleted on_deleted) {
    const auto meeting = repository.get_meeting(meeting_id);
    if (!meeting) {
        return false;
    }
    if (!delete_spool) {
        repository.delete_meeting(meeting_id);
        deleted(*meeting, on_deleted);
        return true;
    }
    const auto spool = validated_spool_path(text_field(*meeting, "spool_dir"), meetings_root);
    std::optional<fs::path> tombstone;
    std::error_code error;
    if (spool && fs::is_directory(*spool, error)) {
        tombstone = tombstone_spool(*spool, meeting_id);
    }
    try {
        repository.delete_meeting(meeting_id);
    } catch (...) {
        if (tombstone) {
            restore_spool(*tombstone, *spool, "Could not restore meeting spool " + meeting_id);
        }
        throw;
    }
    if (tombstone) {
        remove_tree(*tombstone, "Could not purge tombstoned spool for " + meeting_id);
    }
    deleted(*meeting, on_deleted);
    return true;
}

ClearResult clear_meetings(MeetingRepository& repository, const fs::path& meetings_root,
                           bool delete_spools, const std::set<std::string, std::less<>>& skip_ids,
                           MeetingDeleted on_deleted) {
    std::set<std::string> keep_spools;
    ClearResult result;
    for (const Json& meeting : repository.list_meetings()) {
        const std::string meeting_id = text_field(meeting, "id");
        if (meeting_id.empty()) {
            continue;
        }
        const std::string status = py::lower(text_field(meeting, "status"));
        // Live capture rows are never cleared, even when a caller forgets to skip them; a
        // paired computer's meetings are its own to manage.
        if (skip_ids.contains(meeting_id) || status == "active" || status == "paused" ||
            status == "ending" || truthy(get(meeting, "origin_device_id"))) {
            const std::string spool = text_field(meeting, "spool_dir");
            if (!spool.empty()) {
                keep_spools.insert(spool);
            }
            continue;
        }
        if (delete_meeting_data(repository, meeting_id, meetings_root, delete_spools, on_deleted)) {
            result.removed += 1;
        }
    }
    if (delete_spools) {
        purge_orphan_spools(meetings_root, keep_spools);
    }
    return result;
}

} // namespace fv::meeting_state
