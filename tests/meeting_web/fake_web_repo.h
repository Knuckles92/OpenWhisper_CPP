#pragma once

// tests/fakes/meeting_web.py: one active meeting behind a repository that answers the calls the
// dashboard API makes (FakeWebRepo), its tokens, and the store a fake engine serves
// (FakeWebStore's document as a real MeetingStateStore).

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "meeting_state/content.h"
#include "meeting_state/repository.h"
#include "meeting_state/schema.h"
#include "meeting_state/store.h"
#include "meeting_web/dashboard_api.h"
#include "meeting_web/detail/py_value.h"
#include "pyport/text.h"

namespace fv::meeting_web::test_support {

inline constexpr std::string_view kFakeHostToken = "host-secret-token-aaaaaaaaaaaaaaaa";
inline constexpr std::string_view kFakeGuestToken = "guest-secret-token-bbbbbbbbbbbbbbbb";

inline Json fake_web_meeting() {
    return Json{{"id", "m_test"},
                {"title", "Auth Test"},
                {"status", "active"},
                {"started_at", "2026-01-01T00:00:00"},
                {"ended_at", nullptr},
                {"paused_total_s", 0.0},
                {"cloud_enabled", false},
                {"asr_model", "base"},
                {"host_token", kFakeHostToken},
                {"guest_token", kFakeGuestToken},
                {"state_json", "{}"}};
}

// FakeWebStore's document: the active meeting "Auth Test".
inline meeting_state::MeetingState fake_web_state(const std::string& meeting_id = "m_test") {
    meeting_state::MeetingState state;
    state.meeting_id = meeting_id;
    state.title = "Auth Test";
    state.status = "active";
    state.intelligence_online = true;
    return state;
}

class FakeWebRepo : public meeting_state::MeetingRepository, public PastMeetingPages {
public:
    FakeWebRepo() : meeting_(fake_web_meeting()) {
        meetings_.push_back(meeting_);
        chunks_.push_back(Json{{"id", 1},
                               {"meeting_id", "m_test"},
                               {"channel", "loopback"},
                               {"duration_s", 1.0},
                               {"file_path", "/tmp/loopback.wav"}});
    }

    // ---- what the tests set and read (the Python fake's attributes) ----
    void set_meeting(Json meeting) {
        std::lock_guard lock(mutex_);
        meeting_ = std::move(meeting);
    }
    // `_meetings`: the rows the History list shows (not the row get_meeting answers).
    void set_meetings(std::vector<Json> meetings) {
        std::lock_guard lock(mutex_);
        meetings_ = std::move(meetings);
    }
    void set_segments(std::vector<Json> segments) {
        std::lock_guard lock(mutex_);
        segments_ = std::move(segments);
    }
    void set_chunks(std::vector<Json> chunks) {
        std::lock_guard lock(mutex_);
        chunks_ = std::move(chunks);
    }
    // ReportRepo: a second, ended meeting get_meeting answers, whose snapshot writes land in it.
    void set_past(Json past) {
        std::lock_guard lock(mutex_);
        past_ = std::move(past);
    }
    Json past() const {
        std::lock_guard lock(mutex_);
        return past_;
    }
    std::vector<std::string> search_calls() const {
        std::lock_guard lock(mutex_);
        return search_calls_;
    }
    std::vector<std::string> deleted() const {
        std::lock_guard lock(mutex_);
        return deleted_;
    }

    // ---- MeetingRepository ----
    std::optional<Json> get_meeting(std::string_view meeting_id) override {
        std::lock_guard lock(mutex_);
        if (past_.is_object() && past_.value("id", std::string()) == meeting_id) {
            return std::optional<Json>(std::in_place, past_);
        }
        if (meeting_.is_object() && meeting_.value("id", std::string()) == meeting_id) {
            return std::optional<Json>(std::in_place, meeting_);
        }
        return std::nullopt;
    }
    std::vector<Json> list_meetings() override {
        std::lock_guard lock(mutex_);
        return meetings_;
    }
    void update_meeting(std::string_view meeting_id, const Json& fields) override {
        std::lock_guard lock(mutex_);
        if (meeting_.value("id", std::string()) == meeting_id) {
            meeting_.update(fields);
        }
    }
    void rename_meeting(std::string_view, std::string_view) override {}
    void replace_tokens(std::string_view meeting_id, std::string_view host_token,
                        std::string_view guest_token) override {
        std::lock_guard lock(mutex_);
        tokens_ = {std::string(meeting_id), std::string(host_token), std::string(guest_token)};
    }
    // The last replace_tokens call: (meeting id, host token, guest token).
    std::vector<std::string> tokens() const {
        std::lock_guard lock(mutex_);
        return tokens_;
    }
    void delete_meeting(std::string_view meeting_id) override {
        std::lock_guard lock(mutex_);
        deleted_.emplace_back(meeting_id);
    }
    std::vector<Json> search_transcripts(std::string_view query, const std::optional<std::string>&,
                                         std::int64_t) override {
        std::lock_guard lock(mutex_);
        search_calls_.emplace_back(query);
        return {};
    }
    std::vector<Json> get_segments(std::string_view meeting_id, double after_start_s,
                                   std::optional<std::int64_t> limit) override {
        std::lock_guard lock(mutex_);
        std::vector<Json> rows;
        for (const auto& row : segments_) {
            if (row.at("meeting_id").get<std::string>() == meeting_id &&
                row.at("start_s").get<double>() > after_start_s) {
                rows.push_back(row);
            }
        }
        if (limit && *limit > 0 && static_cast<std::size_t>(*limit) < rows.size()) {
            rows.resize(static_cast<std::size_t>(*limit));
        }
        return rows;
    }
    std::vector<Json> get_audio_chunks(std::string_view meeting_id) override {
        std::lock_guard lock(mutex_);
        std::vector<Json> rows;
        for (const auto& row : chunks_) {
            if (row.at("meeting_id").get<std::string>() == meeting_id) {
                rows.push_back(row);
            }
        }
        return rows;
    }
    std::vector<Json> get_segments_page(std::string_view meeting_id,
                                        std::optional<double> cursor_start_s,
                                        const std::optional<std::string>& cursor_id,
                                        std::int64_t limit) override {
        std::vector<Json> rows = get_segments(meeting_id, -1e300, std::nullopt);
        std::ranges::sort(rows, [](const Json& a, const Json& b) {
            return std::pair(a.at("start_s").get<double>(), a.at("id").get<std::string>()) <
                   std::pair(b.at("start_s").get<double>(), b.at("id").get<std::string>());
        });
        if (cursor_start_s && cursor_id) {
            std::erase_if(rows, [&](const Json& row) {
                return !(
                    std::pair(row.at("start_s").get<double>(), row.at("id").get<std::string>()) >
                    std::pair(*cursor_start_s, *cursor_id));
            });
        }
        if (static_cast<std::size_t>(limit) < rows.size()) {
            rows.resize(static_cast<std::size_t>(limit));
        }
        return rows;
    }
    std::optional<Json> get_segment(std::string_view meeting_id,
                                    std::string_view segment_id) override {
        std::lock_guard lock(mutex_);
        for (const auto& row : segments_) {
            if (row.at("meeting_id").get<std::string>() == meeting_id &&
                row.at("id").get<std::string>() == segment_id) {
                return std::optional<Json>(std::in_place, row);
            }
        }
        return std::nullopt;
    }
    std::vector<Json> get_last_segments(std::string_view, std::int64_t) override { return {}; }
    std::vector<Json> list_events(std::string_view, std::optional<std::int64_t>,
                                  std::int64_t) override {
        return {Json{{"seq", 2},
                     {"ts", "2026-01-01T00:00:01"},
                     {"actor_type", "agent"},
                     {"actor_id", "agent"},
                     {"action", "add_item"},
                     {"target_id", "it_1"},
                     {"undoable", true}}};
    }
    // ReportRepo.on_ops_applied and persist_state: the snapshot lands in the past meeting.
    Json on_ops_applied(std::string_view meeting_id, const Json& state,
                        std::vector<meeting_state::OpResult>&, std::string_view,
                        const std::optional<std::string>&) override {
        persist_state(meeting_id, state);
        return Json();
    }
    Json persist_state(std::string_view meeting_id, const Json& state) override {
        std::lock_guard lock(mutex_);
        if (past_.is_object() && past_.value("id", std::string()) == meeting_id) {
            past_["state_json"] = state.dump();
        }
        return Json();
    }
    bool segment_exists(std::string_view, std::string_view) override { return false; }
    std::map<std::string, bool, std::less<>>
    segment_flags(std::string_view, const std::vector<std::string>&) override {
        return {};
    }
    std::optional<Json> get_event(std::string_view, std::int64_t) override { return std::nullopt; }
    bool event_is_undone(std::string_view, std::int64_t) override { return false; }

    void create_meeting(const Json&) override { unused("create_meeting"); }
    void heartbeat(std::string_view, std::int64_t, std::string_view) override {
        unused("heartbeat");
    }
    std::vector<Json> find_interrupted_meetings() override { unused("find_interrupted_meetings"); }
    std::int64_t register_chunk(const Json&) override { unused("register_chunk"); }
    meeting_state::ChunkRegistration register_chunk_if_missing(const Json&) override {
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
    std::int64_t next_chunk_seq(std::string_view, std::string_view) override {
        unused("next_chunk_seq");
    }
    std::int64_t mark_chunks_done(std::string_view) override { unused("mark_chunks_done"); }
    void add_segments(const std::vector<meeting_state::TranscriptSegment>&) override {
        unused("add_segments");
    }
    meeting_state::ChunkCommit
    commit_chunk_transcription(std::string_view, std::int64_t,
                               const std::vector<meeting_state::TranscriptSegment>&) override {
        unused("commit_chunk_transcription");
    }
    void update_segment_speaker(std::string_view, std::string_view,
                                const std::optional<std::string>&, std::string_view,
                                bool) override {
        unused("update_segment_speaker");
    }
    meeting_state::TranscriptReplacement
    replace_final_transcript(std::string_view,
                             const std::vector<meeting_state::TranscriptSegment>&) override {
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

    // ---- PastMeetingPages: list_past_meeting_summaries over _meetings ----
    std::vector<Json>
    list_past_meeting_summaries(const meeting_state::PastMeetingQuery& query) override {
        std::vector<Json> rows = list_meetings();
        std::ranges::sort(rows, [](const Json& a, const Json& b) {
            return std::pair(a.at("started_at").get<std::string>(), a.at("id").get<std::string>()) >
                   std::pair(b.at("started_at").get<std::string>(), b.at("id").get<std::string>());
        });
        if (!query.include_running) {
            std::erase_if(rows, [](const Json& row) {
                const auto status = row.value("status", std::string());
                return status == "active" || status == "paused" || status == "ending";
            });
        }
        if (query.cursor_started_at && query.cursor_id) {
            std::erase_if(rows, [&](const Json& row) {
                return !(std::pair(row.at("started_at").get<std::string>(),
                                   row.at("id").get<std::string>()) <
                         std::pair(*query.cursor_started_at, *query.cursor_id));
            });
        }
        if (!query.started_after.empty()) {
            std::erase_if(rows, [&](const Json& row) {
                return row.at("started_at").get<std::string>() < query.started_after;
            });
        }
        if (!query.query.empty()) {
            const std::string needle = py::lower(query.query);
            std::erase_if(rows, [&](const Json& row) {
                return py::lower(detail::py_repr(row)).find(needle) == std::string::npos;
            });
        }
        if (static_cast<std::size_t>(query.limit) < rows.size()) {
            rows.resize(static_cast<std::size_t>(query.limit));
        }
        for (auto& row : rows) {
            row["content_summary"] = summary(row.at("id").get<std::string>());
        }
        return rows;
    }

private:
    // get_meeting_content_summary through summarize_meeting_content's generic reads.
    Json summary(const std::string& meeting_id) {
        return meeting_state::summarize_meeting_content(*this, meeting_id);
    }

    [[noreturn]] static void unused(const char* method) {
        throw std::logic_error(std::string("the fake web repository has no ") + method);
    }

    mutable std::mutex mutex_;
    Json meeting_;
    Json past_;
    std::vector<Json> meetings_;
    std::vector<Json> segments_;
    std::vector<Json> chunks_;
    std::vector<std::string> search_calls_;
    std::vector<std::string> deleted_;
    std::vector<std::string> tokens_;
};

} // namespace fv::meeting_web::test_support
