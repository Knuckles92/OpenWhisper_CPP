#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

namespace fv {
class Database;
}

// meeting/persist/repository.py's paired-computer queries (record_owners .. replace_meeting_rows,
// count_unfinished_chunks): the meetings a paired computer stored on this one, and whole-meeting
// row copies for moving a meeting between computers. Free functions over the app database, so
// that the MeetingRepository interface does not grow for them. Past Meetings of one paired
// computer are SqlMeetingRepository::list_past_meeting_summaries with PastMeetingQuery::Origin.
// Affinity: thread-safe (Database serializes writers); blocking, never on the GUI thread.
namespace fv::meeting_state {

// One paired computer that stored meetings here. name is the greatest stored
// origin_device_name, or "Unnamed computer" when every one is NULL or empty.
struct RecordOwner {
    std::string id;
    std::string name;
    std::int64_t count = 0;

    friend bool operator==(const RecordOwner&, const RecordOwner&) = default;
};

// One meeting a paired computer stored here, and its spool folder ("" when NULL).
struct OriginSpool {
    std::string meeting_id;
    std::string spool_dir;

    friend bool operator==(const OriginSpool&, const OriginSpool&) = default;
};

// Grouped by origin_device_id, this computer's own meetings (NULL) left out; in SQLite's group
// order.
std::vector<RecordOwner> record_owners(Database& database);

// Badges every meeting `device_id` stored here with its new name; the number of meetings. Ids
// stay; the caller clips the name (Python's name[:120] lives in the record kinds).
int rename_origin(Database& database, std::string_view device_id, std::string_view name);

// Every meeting `device_id` stored here, in table order.
std::vector<OriginSpool> origin_spools(Database& database, std::string_view device_id);

// Every row of one meeting as column objects in table order, for copying it:
// {"session": {...}, "chunks", "segments", "participants", "state_items", "questions",
// "events": [{...}, ...]}, children ordered by id. BOOLEAN columns read as bools, the segment
// embedding as Json::binary, segment text as stored (before term corrections, since the copy
// re-applies them from its own state). Null for an unknown meeting.
Json export_meeting_rows(Database& database, std::string_view meeting_id);

// Writes a whole meeting copied from another computer in one BEGIN IMMEDIATE transaction:
// every child row of `meeting_id` and its session row are deleted explicitly (SQLite does not
// fire the FTS sync triggers on a cascade), then `session_values` is inserted, then `rows`
// ({"chunks", "segments", "participants", "state_items", "questions", "events"}, each optional).
// Chunks get this database's ids: each chunk's "chunk_ref" (its id on the other computer) is
// taken out and segments' chunk_id is mapped through it, NULL for a chunk the copy lacks.
// Columns missing from a row take the ORM's defaults. Throws py::TypeError for a key that is no
// column of its table ("'<key>' is an invalid keyword argument for <Model>") or a row that is
// no object, a py::PyError KeyError for a chunk without "chunk_ref", a py::PyError
// StatementError for a BOOLEAN or BLOB value SQLAlchemy's bind processors refuse ("(builtins.
// TypeError) Not a boolean value: 'yes'"), and sql::Error for a constraint (SQLAlchemy's
// IntegrityError); nothing is kept then. When publish_segments is false, the copy is
// indexed without entering its segments into the live delivery queue.
void replace_meeting_rows(Database& database, std::string_view meeting_id,
                          const Json& session_values, const Json& rows,
                          bool publish_segments = true);

// Chunks of the meeting not yet durably done (asr_status != 'done').
std::int64_t count_unfinished_chunks(Database& database, std::string_view meeting_id);

} // namespace fv::meeting_state
