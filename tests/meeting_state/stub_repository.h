#pragma once

// A MeetingRepository whose every method fails the test unless a subclass overrides it, for the
// Python tests that hand the store a duck-typed repository with two or three methods.

#include <stdexcept>
#include <string>

#include "meeting_state/repository.h"

namespace fv::meeting_state::test_support {

class StubRepository : public MeetingRepository {
public:
    void create_meeting(const Json&) override { unused("create_meeting"); }
    void update_meeting(std::string_view, const Json&) override { unused("update_meeting"); }
    void rename_meeting(std::string_view, std::string_view) override { unused("rename_meeting"); }
    void replace_tokens(std::string_view, std::string_view, std::string_view) override {
        unused("replace_tokens");
    }
    std::optional<Json> get_meeting(std::string_view) override { unused("get_meeting"); }
    std::vector<Json> list_meetings() override { unused("list_meetings"); }
    void delete_meeting(std::string_view) override { unused("delete_meeting"); }
    void heartbeat(std::string_view, std::int64_t, std::string_view) override {
        unused("heartbeat");
    }
    std::vector<Json> find_interrupted_meetings() override { unused("find_interrupted_meetings"); }
    std::int64_t register_chunk(const Json&) override { unused("register_chunk"); }
    ChunkRegistration register_chunk_if_missing(const Json&) override {
        unused("register_chunk_if_missing");
    }
    void set_chunk_status(std::int64_t, std::string_view,
                          std::optional<std::string_view>) override {
        unused("set_chunk_status");
    }
    std::vector<Json> get_pending_chunks(std::string_view, std::optional<std::int64_t>,
                                         const std::vector<std::int64_t>&) override {
        unused("get_pending_chunks");
    }
    std::int64_t count_unfinished_chunks(std::string_view) override {
        unused("count_unfinished_chunks");
    }
    std::int64_t reset_unfinished_chunks(std::string_view, bool) override {
        unused("reset_unfinished_chunks");
    }
    void defer_chunk_after_connection_failure(std::int64_t, std::string_view) override {
        unused("defer_chunk_after_connection_failure");
    }
    std::vector<Json> get_audio_chunks(std::string_view) override { unused("get_audio_chunks"); }
    std::int64_t next_chunk_seq(std::string_view, std::string_view) override {
        unused("next_chunk_seq");
    }
    std::int64_t mark_chunks_done(std::string_view) override { unused("mark_chunks_done"); }
    void add_segments(const std::vector<TranscriptSegment>&) override { unused("add_segments"); }
    ChunkCommit commit_chunk_transcription(std::string_view, std::int64_t,
                                           const std::vector<TranscriptSegment>&) override {
        unused("commit_chunk_transcription");
    }
    std::optional<Json> get_segment(std::string_view, std::string_view) override {
        unused("get_segment");
    }
    bool segment_exists(std::string_view, std::string_view) override { unused("segment_exists"); }
    std::map<std::string, bool, std::less<>>
    segment_flags(std::string_view, const std::vector<std::string>&) override {
        unused("segment_flags");
    }
    void update_segment_speaker(std::string_view, std::string_view,
                                const std::optional<std::string>&, std::string_view,
                                bool) override {
        unused("update_segment_speaker");
    }
    std::vector<Json> get_segments(std::string_view, double, std::optional<std::int64_t>) override {
        unused("get_segments");
    }
    std::vector<Json> get_segments_page(std::string_view, std::optional<double>,
                                        const std::optional<std::string>&, std::int64_t) override {
        unused("get_segments_page");
    }
    std::vector<Json> get_last_segments(std::string_view, std::int64_t) override {
        unused("get_last_segments");
    }
    TranscriptReplacement replace_final_transcript(std::string_view,
                                                   const std::vector<TranscriptSegment>&) override {
        unused("replace_final_transcript");
    }
    std::vector<Json> get_segments_delivery_page(std::string_view, std::int64_t,
                                                 std::int64_t) override {
        unused("get_segments_delivery_page");
    }
    std::int64_t get_delivery_cursor(std::string_view, std::string_view) override {
        unused("get_delivery_cursor");
    }
    void advance_delivery_cursor(std::string_view, std::string_view, std::int64_t) override {
        unused("advance_delivery_cursor");
    }
    std::int64_t get_delivery_high_water(std::string_view) override {
        unused("get_delivery_high_water");
    }
    Json on_ops_applied(std::string_view, const Json&, std::vector<OpResult>&, std::string_view,
                        const std::optional<std::string>&) override {
        unused("on_ops_applied");
    }
    Json persist_state(std::string_view, const Json&) override { unused("persist_state"); }
    std::optional<Json> get_event(std::string_view, std::int64_t) override { unused("get_event"); }
    bool event_is_undone(std::string_view, std::int64_t) override { unused("event_is_undone"); }
    std::vector<Json> list_events(std::string_view, std::optional<std::int64_t>,
                                  std::int64_t) override {
        unused("list_events");
    }
    std::vector<Json> search_transcripts(std::string_view, const std::optional<std::string>&,
                                         std::int64_t) override {
        unused("search_transcripts");
    }

protected:
    [[noreturn]] static void unused(const char* method) {
        throw std::logic_error(std::string("the test repository has no ") + method);
    }
};

} // namespace fv::meeting_state::test_support
