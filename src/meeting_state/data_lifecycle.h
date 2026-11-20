#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>

#include "meeting_state/repository.h"

// meeting/persist/data_lifecycle.py: recoverable deletion of meeting rows and audio spools.
// A spool is renamed aside first ("tombstoned"), which makes the database delete the commit
// point while the audio can still be put back; only after the rows are gone is it removed.
// Affinity: thread-safe as the repository; blocks on the database and the disk.
namespace fv::meeting_state {

// A tombstone older than this is one a crash abandoned, and a purge collects it (seconds).
inline constexpr double DELETION_TOMBSTONE_STALE_S = 60 * 60;
// Name prefix of a spool an in-flight delete renamed aside.
inline constexpr std::string_view TOMBSTONE_PREFIX = ".deleting-";

// Called once a meeting's rows are deleted, with its id and whether a paired computer stored
// it here (its origin_device_id). The app clears the event bus's replay buffer (meeting
// segments carry what was said, and a reconnecting stream must not get them back) and, for a
// meeting of its own, tells the record sync; the callback catches its own failures.
using MeetingDeleted = std::function<void(std::string meeting_id, bool has_origin)>;

// Deletes a meeting and, with delete_spool, its spool. False when there is no such meeting.
// The spool must be a direct child of meetings_root by real path, else
// py::ValueError("meeting spool is outside the configured meetings root") and nothing is
// deleted. The spool is renamed to ".deleting-<id>-<32 hex>" (its mtime refreshed, so a purge
// measures the age of the deletion, not of the last chunk); a failed database delete renames
// it back (unless something took its place) and rethrows; afterwards it is removed (a failure
// is logged). An empty meetings_root or spool_dir deletes the rows only.
bool delete_meeting_data(MeetingRepository& repository, const std::string& meeting_id,
                         const std::filesystem::path& meetings_root, bool delete_spool = true,
                         MeetingDeleted on_deleted = {});

struct ClearResult {
    // Meetings whose rows were deleted.
    std::int64_t removed = 0;
};

// Deletes every historical meeting. Live rows (active, paused, ending), the ids in skip_ids and
// meetings a paired computer stored here (its to manage) stay, with their spools. With
// delete_spools, every other directory under meetings_root that is not a kept spool goes too
// (orphans of earlier keep-recording deletes), and tombstones older than
// DELETION_TOMBSTONE_STALE_S; files and recent tombstones stay. A delete that throws stops the
// clear and propagates.
ClearResult clear_meetings(MeetingRepository& repository,
                           const std::filesystem::path& meetings_root, bool delete_spools,
                           const std::set<std::string, std::less<>>& skip_ids = {},
                           MeetingDeleted on_deleted = {});

} // namespace fv::meeting_state
