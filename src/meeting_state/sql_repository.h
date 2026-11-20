#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "meeting_state/corrections.h"
#include "meeting_state/repository.h"
#include "meeting_state/types.h"

namespace fv {
class Database;
}

// meeting/persist/repository.py: every meeting_* statement behind one seam, over the app
// database (db/schema.h's tables, OpenWhisper's schema; this file never changes the schema).
// Rows are the JSON objects repository.h describes; segment reads apply the meeting's term and
// occurrence corrections to "text" and keep the raw words in "original_text".
// The paired-computer queries (record_owners .. replace_meeting_rows) are origin_rows.h's.
// Affinity: thread-safe (Database serializes writers; reads use pooled readers). Blocking:
// never on the GUI thread.
namespace fv::meeting_state {

// Minimum time IoU for a new transcript row to inherit an old row's speaker label or evidence
// anchor.
inline constexpr double MIN_MATCH_IOU = 0.25;

// Intersection over union of two half-open time intervals (seconds); 0 when they do not overlap.
double interval_iou(double a_start, double a_end, double b_start, double b_end);

// meeting/corrections.py repository_term_rules: the meeting's term rules for ASR engines
// without a live store (recovery, offline finalization), re-read only when the row's state_seq
// changes; {} for an unknown meeting, a null repository or a failed read (logged). The provider
// keeps `repository` alive and may be called from any thread.
std::function<TermRules()> repository_term_rules(std::shared_ptr<MeetingRepository> repository,
                                                 std::string meeting_id);

// list_past_meeting_summaries's keyword arguments.
struct PastMeetingQuery {
    // Which computer's meetings: every one (Python's `...`), this computer's own (None), or
    // those one paired computer stored here (its device id).
    enum class Origin { Any, Local, Device };

    std::int64_t limit = 101; // clamped to 1..501
    std::string query;        // matched literally, case-insensitively (ASCII), when not blank
    Origin origin = Origin::Any;
    std::string origin_device_id; // for Origin::Device
    // Keyset cursor: meetings strictly before (started_at, id); used when both are set.
    std::optional<std::string> cursor_started_at;
    std::optional<std::string> cursor_id;
    bool include_running = false; // active, paused and ending are left out otherwise
    // julianday(started_at) >= julianday(started_after), when not empty.
    std::string started_after;
    std::optional<std::string> meeting_id;
    // String bounds on the stored started_at, when not empty (the History calendar's month).
    std::string started_from;
    std::string started_before;
};

// One past meeting as the History calendar places it.
struct PastMeetingTime {
    std::string id;
    std::string started_at;
    std::optional<std::string> ended_at;
    double paused_total_s = 0.0;
    std::optional<std::string> origin_device_name;

    friend bool operator==(const PastMeetingTime&, const PastMeetingTime&) = default;
};

class SqlMeetingRepository final : public MeetingRepository {
public:
    // `database` must outlive the repository. Timestamps (segment created_at, event ts) come
    // from `clock`.
    explicit SqlMeetingRepository(Database& database, const Clock& clock = system_clock());

    // Meetings. create_meeting and register_chunk take the ORM's keyword arguments: an unknown
    // key is py::TypeError("'<key>' is an invalid keyword argument for MeetingSession"), and a
    // missing NOT NULL column is SQLite's constraint error (sql::Error).
    void create_meeting(const Json& fields) override;
    void update_meeting(std::string_view meeting_id, const Json& fields) override;
    void rename_meeting(std::string_view meeting_id, std::string_view title) override;
    void replace_tokens(std::string_view meeting_id, std::string_view host_token,
                        std::string_view guest_token) override;
    std::optional<Json> get_meeting(std::string_view meeting_id) override;
    std::vector<Json> list_meetings() override;
    // Children are deleted explicitly (not by FK cascade), because SQLite does not fire the
    // FTS sync triggers on a cascade. The feature counter (set_feature_counter) runs first.
    void delete_meeting(std::string_view meeting_id) override;
    void heartbeat(std::string_view meeting_id, std::int64_t pid,
                   std::string_view utc_iso) override;
    std::vector<Json> find_interrupted_meetings() override;

    // A bounded Past Meetings page, newest first, each row with "content_summary"
    // ({has_audio, has_loopback_audio, has_transcript, is_empty, audio_chunks,
    // transcript_segments, can_rerun_speakers, preview_text}) from three aggregate reads.
    std::vector<Json> list_past_meeting_summaries(const PastMeetingQuery& query = {});
    // The content summary of one meeting (running ones included), or the empty summary.
    Json get_meeting_content_summary(std::string_view meeting_id);
    // Every meeting Past Meetings lists, without its snapshot.
    std::vector<PastMeetingTime> list_past_meeting_times();
    // SELECT statements the last list_past_meeting_summaries call ran (1, or 4 with rows).
    int last_summary_query_count() const;

    std::int64_t register_chunk(const Json& fields) override;
    ChunkRegistration register_chunk_if_missing(const Json& fields) override;
    void set_chunk_status(std::int64_t chunk_id, std::string_view status,
                          std::optional<std::string_view> error) override;
    std::vector<Json> get_pending_chunks(std::string_view meeting_id,
                                         std::optional<std::int64_t> limit,
                                         const std::vector<std::int64_t>& exclude_ids) override;
    std::int64_t count_unfinished_chunks(std::string_view meeting_id) override;
    std::int64_t reset_unfinished_chunks(std::string_view meeting_id, bool force) override;
    void defer_chunk_after_connection_failure(std::int64_t chunk_id,
                                              std::string_view error) override;
    std::vector<Json> get_audio_chunks(std::string_view meeting_id) override;
    std::int64_t next_chunk_seq(std::string_view meeting_id, std::string_view channel) override;
    std::int64_t mark_chunks_done(std::string_view meeting_id) override;

    void add_segments(const std::vector<TranscriptSegment>& segments) override;
    ChunkCommit commit_chunk_transcription(std::string_view meeting_id, std::int64_t chunk_id,
                                           const std::vector<TranscriptSegment>& segments) override;
    std::optional<Json> get_segment(std::string_view meeting_id,
                                    std::string_view segment_id) override;
    bool segment_exists(std::string_view meeting_id, std::string_view segment_id) override;
    std::map<std::string, bool, std::less<>>
    segment_flags(std::string_view meeting_id,
                  const std::vector<std::string>& segment_ids) override;
    void update_segment_speaker(std::string_view meeting_id, std::string_view segment_id,
                                const std::optional<std::string>& participant_id,
                                std::string_view source, bool pinned) override;
    std::vector<Json> get_segments(std::string_view meeting_id, double after_start_s = -1.0,
                                   std::optional<std::int64_t> limit = std::nullopt) override;
    std::vector<Json> get_segments_page(std::string_view meeting_id,
                                        std::optional<double> cursor_start_s,
                                        const std::optional<std::string>& cursor_id,
                                        std::int64_t limit = 500) override;
    std::vector<Json> get_last_segments(std::string_view meeting_id, std::int64_t count) override;
    // Unpinned live rows are deleted; pinned rows keep their speaker through a time-IoU match
    // onto the new rows, and unmatched pinned rows stay. Unpinned live labels only fill new rows
    // the offline pass left unlabeled. sg_ evidence on cards, questions, pulses and the summary
    // is remapped when a unique IoU match exists.
    TranscriptReplacement
    replace_final_transcript(std::string_view meeting_id,
                             const std::vector<TranscriptSegment>& segments) override;

    std::vector<Json> get_segments_delivery_page(std::string_view meeting_id,
                                                 std::int64_t after_seq = 0,
                                                 std::int64_t limit = 300) override;
    std::int64_t get_delivery_cursor(std::string_view meeting_id,
                                     std::string_view consumer) override;
    void advance_delivery_cursor(std::string_view meeting_id, std::string_view consumer,
                                 std::int64_t seq) override;
    std::int64_t get_delivery_high_water(std::string_view meeting_id) override;

    // One BEGIN IMMEDIATE transaction: one-occurrence corrections revalidated against the raw
    // row (or the same batch's revised text), then an audit event and the mirror of each
    // result, the title column on an explicit set_title, and the snapshot (ensure_ascii off).
    Json on_ops_applied(std::string_view meeting_id, const Json& state,
                        std::vector<OpResult>& results, std::string_view actor_type,
                        const std::optional<std::string>& actor_id) override;
    Json persist_state(std::string_view meeting_id, const Json& state) override;
    std::optional<Json> get_event(std::string_view meeting_id, std::int64_t seq) override;
    bool event_is_undone(std::string_view meeting_id, std::int64_t seq) override;
    std::vector<Json> list_events(std::string_view meeting_id,
                                  std::optional<std::int64_t> before_seq = std::nullopt,
                                  std::int64_t limit = 100) override;

    std::vector<Json> search_transcripts(std::string_view query,
                                         const std::optional<std::string>& exclude_meeting_id,
                                         std::int64_t limit = 200) override;

    // Passages a semantic ranking may judge: {segment_id, meeting_id, start_s, end_s, text (raw),
    // title, started_at}. Half the limit (clamped to 1..48) goes to full-text matches of the
    // query's words longer than two characters, stop words left out (an OR of at most 20, best
    // bm25 first); the rest to the longest passage of each minute of each meeting, newest
    // passages of every meeting first, so meetings without a shared word still get judged.
    // shared_only keeps to meetings that had AI insights on (cloud_enabled), the only ones whose
    // text may leave this computer. Without the search index only the recent passages are
    // returned. Defined in detail/sql_candidates.cpp; not part of MeetingRepository (lead
    // amendment 26): consumers reach it through a port of their own.
    std::vector<Json> search_candidates(std::string_view query,
                                        const std::optional<std::string>& exclude_meeting_id = {},
                                        std::int64_t limit = 48, bool shared_only = true);

    // The meeting's term rules as {term: replacement}, re-read when state_seq changes ({} on
    // failure). The provider refers to this repository, which must outlive it.
    std::function<Json()> term_rules_provider(std::string meeting_id);

    // dictation_stats.count_features, called (best effort) before a meeting is deleted so Stats
    // keep counting it.
    void set_feature_counter(std::function<void()> count_features);

private:
    Database& database_;
    const Clock& clock_;
    mutable std::mutex mutex_; // guards the two members below
    std::function<void()> count_features_;
    int last_summary_queries_ = 0;
};

namespace detail {

// _old_segment_items: what the matching reads of a live row.
struct OldSegment {
    std::string id;
    double start_s = 0.0;
    double end_s = 0.0;
    bool pinned = false;
    std::optional<std::string> speaker_participant_id;
    std::string speaker_source;
    std::string channel;
};

// _match_live_to_offline: pairs old and new rows one to one by best time IoU on the same
// channel (score, then old index, then new index, all descending), copying the speaker where it
// should win (a pin always; an unpinned label only into an unlabeled row). Returns old id ->
// new id in pairing order and the matched old indices.
std::pair<std::vector<std::pair<std::string, std::string>>, std::set<std::size_t>>
match_live_to_offline(const std::vector<OldSegment>& old_items,
                      std::vector<TranscriptSegment>& new_items);

// _remap_state_evidence: rewrites sg_ evidence of every card item (clearing its
// citation_check), every question, the live pulses (a pulse whose anchor dies is dropped) and
// the rolling summary onto the new transcript, in place.
void remap_state_evidence(Json& state,
                          const std::vector<std::pair<std::string, std::string>>& id_map,
                          const std::vector<OldSegment>& old_items,
                          const std::vector<TranscriptSegment>& new_items,
                          const std::set<std::string, std::less<>>& retained_ids);

// _content_summary: the preview is clipped to 100 code points (right-stripped, then "…").
Json content_summary(std::int64_t audio_count, bool has_loopback, std::int64_t segment_count,
                     std::string preview);

// _search_filter's LIKE pattern: %needle% with \, % and _ escaped by a backslash.
std::string search_like_pattern(std::string_view needle);

} // namespace detail

} // namespace fv::meeting_state
