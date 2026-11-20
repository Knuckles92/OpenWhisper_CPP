#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "meeting_state/repository.h"

namespace fv::meeting_state::testing {

// MeetingRepository in memory, with the SQL repository's rules: column defaults and NOT NULL
// and UNIQUE failures, the chunk lifecycle (done is terminal, processing spends an attempt,
// blocked and exhausted chunks stay put), segment reads with the meeting's corrections
// applied, commit-order delivery sequences that never repeat, audit events and undo marks, and
// the participant check of speaker reassignment. Two parts are simpler than SQL:
// replace_final_transcript deletes the unpinned rows, keeps the pinned ones and inserts the
// new segments without matching speakers or remapping evidence by time (its id_map is empty),
// and search_transcripts matches every whitespace term as a case-insensitive substring of the
// raw text, with the text as the snippet. Timestamps come from the clock.
// Affinity: thread-safe.
class FakeMeetingRepository final : public MeetingRepository {
public:
    explicit FakeMeetingRepository(const Clock& clock = system_clock());

    void create_meeting(const Json& fields) override;
    void update_meeting(std::string_view meeting_id, const Json& fields) override;
    void rename_meeting(std::string_view meeting_id, std::string_view title) override;
    void replace_tokens(std::string_view meeting_id, std::string_view host_token,
                        std::string_view guest_token) override;
    std::optional<Json> get_meeting(std::string_view meeting_id) override;
    std::vector<Json> list_meetings() override;
    void delete_meeting(std::string_view meeting_id) override;
    void heartbeat(std::string_view meeting_id, std::int64_t pid,
                   std::string_view utc_iso) override;
    std::vector<Json> find_interrupted_meetings() override;

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
    std::vector<Json> get_segments(std::string_view meeting_id, double after_start_s,
                                   std::optional<std::int64_t> limit) override;
    std::vector<Json> get_segments_page(std::string_view meeting_id,
                                        std::optional<double> cursor_start_s,
                                        const std::optional<std::string>& cursor_id,
                                        std::int64_t limit) override;
    std::vector<Json> get_last_segments(std::string_view meeting_id, std::int64_t count) override;
    TranscriptReplacement
    replace_final_transcript(std::string_view meeting_id,
                             const std::vector<TranscriptSegment>& segments) override;

    std::vector<Json> get_segments_delivery_page(std::string_view meeting_id,
                                                 std::int64_t after_seq,
                                                 std::int64_t limit) override;
    std::int64_t get_delivery_cursor(std::string_view meeting_id,
                                     std::string_view consumer) override;
    void advance_delivery_cursor(std::string_view meeting_id, std::string_view consumer,
                                 std::int64_t seq) override;
    std::int64_t get_delivery_high_water(std::string_view meeting_id) override;

    Json on_ops_applied(std::string_view meeting_id, const Json& state,
                        std::vector<OpResult>& results, std::string_view actor_type,
                        const std::optional<std::string>& actor_id) override;
    Json persist_state(std::string_view meeting_id, const Json& state) override;
    std::optional<Json> get_event(std::string_view meeting_id, std::int64_t seq) override;
    bool event_is_undone(std::string_view meeting_id, std::int64_t seq) override;
    std::vector<Json> list_events(std::string_view meeting_id,
                                  std::optional<std::int64_t> before_seq,
                                  std::int64_t limit) override;

    std::vector<Json> search_transcripts(std::string_view query,
                                         const std::optional<std::string>& exclude_meeting_id,
                                         std::int64_t limit) override;

    // Failure injection: the next `times` register_chunk calls throw std::runtime_error
    // ("database is locked"); on_ops_applied and persist_state throw std::runtime_error with
    // the message while one is set.
    void fail_register_chunk(int times);
    void fail_on_ops_applied(std::optional<std::string> message);
    void fail_persist_state(std::optional<std::string> message);

    // What on_ops_applied was given, in call order.
    struct OpsAppliedCall {
        std::string meeting_id;
        Json state;
        std::vector<OpResult> results;
        std::string actor_type;
        std::optional<std::string> actor_id;
    };
    std::vector<OpsAppliedCall> ops_applied_calls() const;
    // Mirrored rows (as written by on_ops_applied) and every audit event, in write order.
    std::optional<Json> mirrored_item(std::string_view meeting_id, std::string_view item_id) const;
    std::optional<Json> mirrored_question(std::string_view meeting_id,
                                          std::string_view question_id) const;
    std::optional<Json> mirrored_participant(std::string_view meeting_id,
                                             std::string_view participant_id) const;
    std::vector<Json> events(std::string_view meeting_id) const;

private:
    struct SegmentRow {
        Json row; // the raw columns; text is the raw text
        std::optional<std::vector<std::uint8_t>> embedding;
    };
    struct Delivery {
        std::int64_t seq = 0;
        std::string segment_id;
        std::string meeting_id;
    };
    struct Event {
        std::string meeting_id;
        Json row;
    };
    using Rows = std::map<std::string, Json, std::less<>>;

    Json* meeting_row(std::string_view meeting_id);
    Json* chunk_row(std::int64_t chunk_id);
    SegmentRow* segment_row(std::string_view segment_id);
    Json segment_dict(const SegmentRow& row) const;
    std::vector<const SegmentRow*> meeting_segments(std::string_view meeting_id) const;
    std::int64_t insert_chunk(const Json& fields);
    void insert_segment(const TranscriptSegment& segment, const std::string& created);
    void erase_segment(std::string_view segment_id);
    void mirror_effect(std::string_view meeting_id, OpResult& result);

    const Clock& clock_;
    mutable std::mutex mutex_;
    std::vector<Json> meetings_;
    std::vector<Json> chunks_;
    std::int64_t next_chunk_id_ = 1;
    std::vector<SegmentRow> segments_; // insertion (rowid) order
    std::vector<Delivery> deliveries_;
    std::int64_t next_delivery_seq_ = 1;
    std::map<std::pair<std::string, std::string>, std::int64_t> cursors_;
    std::vector<Event> events_;
    std::map<std::string, Rows, std::less<>> items_;
    std::map<std::string, Rows, std::less<>> questions_;
    std::map<std::string, Rows, std::less<>> participants_;
    std::vector<OpsAppliedCall> ops_applied_calls_;
    int register_failures_ = 0;
    std::optional<std::string> ops_applied_failure_;
    std::optional<std::string> persist_failure_;
};

} // namespace fv::meeting_state::testing
