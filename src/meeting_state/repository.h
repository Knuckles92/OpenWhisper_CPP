#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "meeting_state/types.h"

// meeting/interfaces.py MeetingRepository, plus the methods the engine calls outside the
// protocol: every statement of Meeting Mode persistence behind one seam. Rows are JSON objects
// with the keys of OpenWhisper's dicts:
//   meeting  id, title, status, started_at, ended_at, paused_total_s, host_token, guest_token,
//            cloud_enabled, asr_model, asr_remote_json, agent_provider, agent_model,
//            agent_endpoint_json, spool_dir, state_json, state_seq, app_pid,
//            app_heartbeat_at, origin_device_id, origin_device_name
//   chunk    id, meeting_id, channel, seq, file_path, start_s, duration_s, sample_rate,
//            asr_status (pending | processing | done | failed | blocked), asr_attempts,
//            asr_error
//   segment  id, meeting_id, chunk_id, channel, start_s, end_s, text (term and occurrence
//            corrections of the meeting's state applied), original_text (raw), speaker_
//            participant_id, speaker_source, speaker_pinned, created_at
//   event    seq, ts, actor_type, actor_id, action, target_id, payload, inverse
// Failures are exceptions: py::ValueError with Python's message where Python raises
// ValueError, StaleCorrectionSelection from on_ops_applied, std::runtime_error for storage.
// Affinity: implementations are thread-safe (the engine, agents, the web server and the GUI
// all call in).
namespace fv::meeting_state {

using Json = fv::Json;

// A chunk is retried until it has been handed to ASR this many times.
inline constexpr std::int64_t MAX_CHUNK_ATTEMPTS = 3;

struct ChunkRegistration {
    std::int64_t id = 0;
    bool created = false;
};

// Stored rows of a chunk (ordered by start_s, id) and whether this call committed them.
struct ChunkCommit {
    std::vector<Json> rows;
    bool committed = false;
};

// Stored new and kept rows (by start_s, id), deleted ids and the old -> new id map.
struct TranscriptReplacement {
    std::vector<Json> rows;
    std::vector<std::string> deleted_ids;
    std::vector<std::pair<std::string, std::string>> id_map;
};

class MeetingRepository {
public:
    virtual ~MeetingRepository() = default;

    // Meetings. `fields` are meeting-row columns.
    virtual void create_meeting(const Json& fields) = 0;
    // An unknown meeting is a logged no-op.
    virtual void update_meeting(std::string_view meeting_id, const Json& fields) = 0;
    // Sets the title column and the snapshot's title in one transaction; normalize_title's
    // py::ValueError for an invalid title, py::ValueError("unknown meeting '<id>'").
    virtual void rename_meeting(std::string_view meeting_id, std::string_view title) = 0;
    virtual void replace_tokens(std::string_view meeting_id, std::string_view host_token,
                                std::string_view guest_token) = 0;
    virtual std::optional<Json> get_meeting(std::string_view meeting_id) = 0;
    // Newest started_at first.
    virtual std::vector<Json> list_meetings() = 0;
    // The meeting and every child row.
    virtual void delete_meeting(std::string_view meeting_id) = 0;
    // app_pid and app_heartbeat_at (UTC ISO text).
    virtual void heartbeat(std::string_view meeting_id, std::int64_t pid,
                           std::string_view utc_iso) = 0;
    // Live or recovering meetings (active, paused, ending, needs_recovery) and any meeting with
    // a chunk that is not done.
    virtual std::vector<Json> find_interrupted_meetings() = 0;

    // Audio chunks. `fields` are chunk-row columns (asr_status defaults to pending).
    virtual std::int64_t register_chunk(const Json& fields) = 0;
    // Idempotent on (meeting_id, channel, seq).
    virtual ChunkRegistration register_chunk_if_missing(const Json& fields) = 0;
    // done is terminal; entering processing spends an attempt; an error is kept to 2000
    // characters.
    virtual void set_chunk_status(std::int64_t chunk_id, std::string_view status,
                                  std::optional<std::string_view> error) = 0;
    // pending, failed and processing chunks with attempts left, by (start_s, id); a limit of 0
    // or less returns nothing.
    virtual std::vector<Json> get_pending_chunks(std::string_view meeting_id,
                                                 std::optional<std::int64_t> limit,
                                                 const std::vector<std::int64_t>& exclude_ids) = 0;
    virtual std::int64_t count_unfinished_chunks(std::string_view meeting_id) = 0;
    // Back to pending; without force, blocked and exhausted chunks stay; force also clears
    // attempts and errors.
    virtual std::int64_t reset_unfinished_chunks(std::string_view meeting_id, bool force) = 0;
    // Back to pending without spending an attempt (a remote disconnect).
    virtual void defer_chunk_after_connection_failure(std::int64_t chunk_id,
                                                      std::string_view error) = 0;
    // By (start_s, channel, seq).
    virtual std::vector<Json> get_audio_chunks(std::string_view meeting_id) = 0;
    virtual std::int64_t next_chunk_seq(std::string_view meeting_id, std::string_view channel) = 0;
    // Every unfinished chunk done; the number changed.
    virtual std::int64_t mark_chunks_done(std::string_view meeting_id) = 0;

    // Segments.
    virtual void add_segments(const std::vector<TranscriptSegment>& segments) = 0;
    // Stores a chunk's segments and marks it done atomically; a chunk already done returns its
    // rows uncommitted.
    virtual ChunkCommit
    commit_chunk_transcription(std::string_view meeting_id, std::int64_t chunk_id,
                               const std::vector<TranscriptSegment>& segments) = 0;
    virtual std::optional<Json> get_segment(std::string_view meeting_id,
                                            std::string_view segment_id) = 0;
    virtual bool segment_exists(std::string_view meeting_id, std::string_view segment_id) = 0;
    // {segment_id: speaker_pinned} for the ids that exist.
    virtual std::map<std::string, bool, std::less<>>
    segment_flags(std::string_view meeting_id, const std::vector<std::string>& segment_ids) = 0;
    // py::ValueError("participant does not belong to meeting") for a participant that is not
    // one of the meeting's.
    virtual void update_segment_speaker(std::string_view meeting_id, std::string_view segment_id,
                                        const std::optional<std::string>& participant_id,
                                        std::string_view source, bool pinned) = 0;
    // start_s strictly after after_start_s (Python's default -1.0), by (start_s, id); an
    // empty or zero limit is unbounded.
    virtual std::vector<Json> get_segments(std::string_view meeting_id, double after_start_s,
                                           std::optional<std::int64_t> limit) = 0;
    // A keyset page after (cursor_start_s, cursor_id); limit clamped to 1..1001 (Python's
    // default 500).
    virtual std::vector<Json> get_segments_page(std::string_view meeting_id,
                                                std::optional<double> cursor_start_s,
                                                const std::optional<std::string>& cursor_id,
                                                std::int64_t limit) = 0;
    // The last `count` segments, in chronological order.
    virtual std::vector<Json> get_last_segments(std::string_view meeting_id,
                                                std::int64_t count) = 0;
    // Replaces the live draft with an offline transcript (pinned rows keep their speakers).
    virtual TranscriptReplacement
    replace_final_transcript(std::string_view meeting_id,
                             const std::vector<TranscriptSegment>& segments) = 0;

    // Transcript delivery to the agent consumers ("cards", "notes"), in commit order. Each
    // segment row of a page carries its "delivery_seq"; limit clamped to 1..301 (default 300).
    virtual std::vector<Json> get_segments_delivery_page(std::string_view meeting_id,
                                                         std::int64_t after_seq,
                                                         std::int64_t limit) = 0;
    // py::ValueError for another consumer.
    virtual std::int64_t get_delivery_cursor(std::string_view meeting_id,
                                             std::string_view consumer) = 0;
    // Never moves a cursor back.
    virtual void advance_delivery_cursor(std::string_view meeting_id, std::string_view consumer,
                                         std::int64_t seq) = 0;
    virtual std::int64_t get_delivery_high_water(std::string_view meeting_id) = 0;

    // State write-through: one audit event per result, the mirrors of their effects (a
    // segment_text effect gains the stored "segment" row), the title column on set_title, and
    // the snapshot. Returns the snapshot reconciled with the stored title (null when the
    // implementation reconciles nothing). Throws StaleCorrectionSelection when a
    // one-occurrence correction no longer matches its passage.
    virtual Json on_ops_applied(std::string_view meeting_id, const Json& state,
                                std::vector<OpResult>& results, std::string_view actor_type,
                                const std::optional<std::string>& actor_id) = 0;
    // Status and snapshot without overwriting an independently edited title.
    virtual Json persist_state(std::string_view meeting_id, const Json& state) = 0;
    virtual std::optional<Json> get_event(std::string_view meeting_id, std::int64_t seq) = 0;
    // Whether an "undo:<seq>" event exists.
    virtual bool event_is_undone(std::string_view meeting_id, std::int64_t seq) = 0;
    // Newest first, without inverse but with "undoable"; limit clamped to 1..500 (default 100).
    virtual std::vector<Json> list_events(std::string_view meeting_id,
                                          std::optional<std::int64_t> before_seq,
                                          std::int64_t limit) = 0;

    // Full-text search over every transcript, newest meeting first: {segment_id, meeting_id,
    // start_s, end_s, text, title, started_at, snippet}; [] for a blank query or on failure.
    virtual std::vector<Json>
    search_transcripts(std::string_view query, const std::optional<std::string>& exclude_meeting_id,
                       std::int64_t limit) = 0;

    // Python's state_metadata_only: the write-through methods reconcile only the title, so a
    // store adopts that field instead of re-reading the whole snapshot.
    virtual bool state_metadata_only() const { return true; }
};

} // namespace fv::meeting_state
