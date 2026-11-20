#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "meeting_state/corrections.h"
#include "meeting_state/types.h"
#include "sql/db.h"

// What the SQL repository's translation units share: Python values to SQLite values and back
// (SQLAlchemy's column types), the row dicts of repository.py, and the per-call cache of
// correction rules _segment_to_dict keeps in session.info.
namespace fv::meeting_state::detail {

// A bound parameter: null, bool (as 0/1), integer, float or text. Containers cannot be bound,
// as pysqlite refuses them (std::runtime_error).
sql::Value to_value(const Json& value);
// A column read as Python sees it: NULL as null, INTEGER, REAL, TEXT as they are.
Json from_value(const sql::Value& value);
// A BOOLEAN column: SQLAlchemy's int-to-bool processor (NULL stays null).
Json bool_from_value(const sql::Value& value);
// The column as text (NULL and other storage classes as ""), for keys and comparisons.
std::string text_value(const sql::Value& value);
std::int64_t int_value(const sql::Value& value);
double real_value(const sql::Value& value);

// The meeting_sessions columns in _session_to_dict's order.
inline constexpr std::string_view kSessionColumns =
    "meeting_sessions.id, meeting_sessions.title, meeting_sessions.status, "
    "meeting_sessions.started_at, meeting_sessions.ended_at, meeting_sessions.paused_total_s, "
    "meeting_sessions.host_token, meeting_sessions.guest_token, meeting_sessions.cloud_enabled, "
    "meeting_sessions.asr_model, meeting_sessions.asr_remote_json, "
    "meeting_sessions.agent_provider, meeting_sessions.agent_model, "
    "meeting_sessions.agent_endpoint_json, meeting_sessions.spool_dir, "
    "meeting_sessions.state_json, meeting_sessions.state_seq, meeting_sessions.app_pid, "
    "meeting_sessions.app_heartbeat_at, meeting_sessions.origin_device_id, "
    "meeting_sessions.origin_device_name";
inline constexpr int kSessionColumnCount = 21;

inline constexpr std::string_view kChunkColumns =
    "meeting_audio_chunks.id, meeting_audio_chunks.meeting_id, meeting_audio_chunks.channel, "
    "meeting_audio_chunks.seq, meeting_audio_chunks.file_path, meeting_audio_chunks.start_s, "
    "meeting_audio_chunks.duration_s, meeting_audio_chunks.sample_rate, "
    "meeting_audio_chunks.asr_status, meeting_audio_chunks.asr_attempts, "
    "meeting_audio_chunks.asr_error";
inline constexpr int kChunkColumnCount = 11;

inline constexpr std::string_view kSegmentColumns =
    "meeting_segments.id, meeting_segments.meeting_id, meeting_segments.chunk_id, "
    "meeting_segments.channel, meeting_segments.start_s, meeting_segments.end_s, "
    "meeting_segments.text, meeting_segments.speaker_participant_id, "
    "meeting_segments.speaker_source, meeting_segments.speaker_pinned, "
    "meeting_segments.created_at";
inline constexpr int kSegmentColumnCount = 11;

// _session_to_dict and _chunk_to_dict of the current row, from column `first` on.
Json session_dict(const sql::Statement& statement, int first = 0);
Json chunk_dict(const sql::Statement& statement, int first = 0);

// _segment_to_dict: the raw columns plus the text read through the meeting's term and
// occurrence corrections. Rules are read once per meeting and call (Python caches them in the
// session), from the connection's view, so a write transaction sees its own state.
class SegmentReader {
public:
    explicit SegmentReader(sql::Connection& connection) : connection_(connection) {}

    Json dict(const sql::Statement& statement, int first = 0);
    // Every row of `sql` (selecting kSegmentColumns) with its parameters bound.
    std::vector<Json> rows(std::string_view sql, const std::vector<sql::Value>& params);
    // The row of one id in any meeting, or nullopt.
    std::optional<Json> by_id(std::string_view segment_id);

private:
    const std::pair<TermRules, ScopedRules>& rules_for(const std::string& meeting_id);

    sql::Connection& connection_;
    std::map<std::string, std::pair<TermRules, ScopedRules>, std::less<>> cache_;
};

// Binds every parameter by position (1-based).
void bind_all(sql::Statement& statement, const std::vector<sql::Value>& params);
// "?, ?, ?" for n parameters.
std::string placeholders(std::size_t count);

// The segment upsert of session.merge(MeetingSegment(...)): an existing id is updated in place
// (no new delivery sequence), a new one inserted.
void merge_segment(sql::Connection& connection, const TranscriptSegment& segment,
                   const std::string& created_at);

// TranscriptSegment's embedding as a BLOB parameter.
sql::Value embedding_value(const std::optional<std::vector<std::uint8_t>>& embedding);

} // namespace fv::meeting_state::detail
