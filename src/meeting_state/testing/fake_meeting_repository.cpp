#include "meeting_state/testing/fake_meeting_repository.h"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "history/titles.h"
#include "meeting_state/corrections.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/schema.h"
#include "meeting_state/time_utils.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::meeting_state::testing {

namespace {

using detail::get;
using detail::truthy;

constexpr std::array<std::string_view, 21> kMeetingColumns = {"id",
                                                              "title",
                                                              "status",
                                                              "started_at",
                                                              "ended_at",
                                                              "paused_total_s",
                                                              "host_token",
                                                              "guest_token",
                                                              "cloud_enabled",
                                                              "asr_model",
                                                              "asr_remote_json",
                                                              "agent_provider",
                                                              "agent_model",
                                                              "agent_endpoint_json",
                                                              "spool_dir",
                                                              "state_json",
                                                              "state_seq",
                                                              "app_pid",
                                                              "app_heartbeat_at",
                                                              "origin_device_id",
                                                              "origin_device_name"};
constexpr std::array<std::string_view, 6> kMeetingRequired = {
    "id", "status", "started_at", "host_token", "guest_token", "spool_dir"};
constexpr std::array<std::string_view, 11> kChunkColumns = {
    "id",         "meeting_id",  "channel",    "seq",          "file_path", "start_s",
    "duration_s", "sample_rate", "asr_status", "asr_attempts", "asr_error"};
constexpr std::array<std::string_view, 7> kChunkRequired = {
    "meeting_id", "channel", "seq", "file_path", "start_s", "duration_s", "sample_rate"};

template <std::size_t N>
bool listed(const std::array<std::string_view, N>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string text_of(const Json& value) {
    return value.is_string() ? value.get<std::string>() : detail::py_str(value);
}

double number_of(const Json& value) {
    return value.is_number() ? value.get<double>() : 0.0;
}

std::int64_t integer_of(const Json& value) {
    return value.is_number() ? value.get<std::int64_t>() : 0;
}

bool is(const Json& value, std::string_view text) {
    return value.is_string() && value.get_ref<const std::string&>() == text;
}

Json meeting_defaults() {
    Json row = Json::object();
    for (const auto column : kMeetingColumns) {
        row[std::string(column)] = nullptr;
    }
    row["title"] = "";
    row["paused_total_s"] = 0.0;
    row["cloud_enabled"] = false;
    row["state_seq"] = 0;
    return row;
}

Json segment_columns(const TranscriptSegment& segment, const std::string& created) {
    Json row = Json::object();
    row["id"] = segment.segment_id;
    row["meeting_id"] = segment.meeting_id;
    row["chunk_id"] = segment.chunk_id ? Json(*segment.chunk_id) : Json();
    row["channel"] = segment.channel;
    row["start_s"] = segment.start_s;
    row["end_s"] = segment.end_s;
    row["text"] = segment.text;
    row["speaker_participant_id"] =
        segment.speaker_participant_id ? Json(*segment.speaker_participant_id) : Json();
    row["speaker_source"] = segment.speaker_source;
    row["speaker_pinned"] = segment.speaker_pinned;
    row["created_at"] = created;
    return row;
}

// ORDER BY start_s, id.
bool by_time(const Json& a, const Json& b) {
    const double sa = number_of(a.at("start_s"));
    const double sb = number_of(b.at("start_s"));
    if (sa != sb) {
        return sa < sb;
    }
    return a.at("id").get_ref<const std::string&>() < b.at("id").get_ref<const std::string&>();
}

std::string error_text(std::string_view error) {
    return py::slice(error, 0, 2000);
}

} // namespace

FakeMeetingRepository::FakeMeetingRepository(const Clock& clock) : clock_(clock) {}

// Lookups (the lock is held by the caller).

Json* FakeMeetingRepository::meeting_row(std::string_view meeting_id) {
    for (auto& row : meetings_) {
        if (is(row.at("id"), meeting_id)) {
            return &row;
        }
    }
    return nullptr;
}

Json* FakeMeetingRepository::chunk_row(std::int64_t chunk_id) {
    for (auto& row : chunks_) {
        if (row.at("id").get<std::int64_t>() == chunk_id) {
            return &row;
        }
    }
    return nullptr;
}

FakeMeetingRepository::SegmentRow* FakeMeetingRepository::segment_row(std::string_view id) {
    for (auto& row : segments_) {
        if (is(row.row.at("id"), id)) {
            return &row;
        }
    }
    return nullptr;
}

Json FakeMeetingRepository::segment_dict(const SegmentRow& segment) const {
    const Json& row = segment.row;
    TermRules rules;
    ScopedRules scoped;
    for (const auto& meeting : meetings_) {
        if (meeting.at("id") == row.at("meeting_id")) {
            const auto state = parse_state_json(meeting.at("state_json"));
            if (state) {
                rules = term_rules(*state);
                scoped = occurrence_rules(*state);
            }
            break;
        }
    }
    const std::string& raw = row.at("text").get_ref<const std::string&>();
    Json out = Json::object();
    out["id"] = row.at("id");
    out["meeting_id"] = row.at("meeting_id");
    out["chunk_id"] = row.at("chunk_id");
    out["channel"] = row.at("channel");
    out["start_s"] = row.at("start_s");
    out["end_s"] = row.at("end_s");
    out["text"] =
        correct_segment_text(raw, row.at("id").get_ref<const std::string&>(), rules, scoped);
    out["original_text"] = raw;
    out["speaker_participant_id"] = row.at("speaker_participant_id");
    out["speaker_source"] = row.at("speaker_source");
    out["speaker_pinned"] = row.at("speaker_pinned");
    out["created_at"] = row.at("created_at");
    return out;
}

std::vector<const FakeMeetingRepository::SegmentRow*>
FakeMeetingRepository::meeting_segments(std::string_view meeting_id) const {
    std::vector<const SegmentRow*> rows;
    for (const auto& row : segments_) {
        if (is(row.row.at("meeting_id"), meeting_id)) {
            rows.push_back(&row);
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const SegmentRow* a, const SegmentRow* b) {
        return by_time(a->row, b->row);
    });
    return rows;
}

void FakeMeetingRepository::insert_segment(const TranscriptSegment& segment,
                                           const std::string& created) {
    segments_.push_back({segment_columns(segment, created), segment.embedding});
    deliveries_.push_back({next_delivery_seq_++, segment.segment_id, segment.meeting_id});
}

void FakeMeetingRepository::erase_segment(std::string_view segment_id) {
    std::erase_if(segments_,
                  [&](const SegmentRow& row) { return is(row.row.at("id"), segment_id); });
    std::erase_if(deliveries_, [&](const Delivery& d) { return d.segment_id == segment_id; });
}

// Meetings

void FakeMeetingRepository::create_meeting(const Json& fields) {
    std::lock_guard lock(mutex_);
    Json row = meeting_defaults();
    for (const auto& [key, value] : fields.items()) {
        if (!listed(kMeetingColumns, key)) {
            throw py::TypeError("'" + key + "' is an invalid keyword argument for MeetingSession");
        }
        row[key] = value;
    }
    for (const auto column : kMeetingRequired) {
        if (row.at(std::string(column)).is_null()) {
            throw std::runtime_error("NOT NULL constraint failed: meeting_sessions." +
                                     std::string(column));
        }
    }
    if (meeting_row(text_of(row.at("id"))) != nullptr) {
        throw std::runtime_error("UNIQUE constraint failed: meeting_sessions.id");
    }
    meetings_.push_back(std::move(row));
}

void FakeMeetingRepository::update_meeting(std::string_view meeting_id, const Json& fields) {
    std::lock_guard lock(mutex_);
    Json* row = meeting_row(meeting_id);
    if (row == nullptr) {
        return;
    }
    for (const auto& [key, value] : fields.items()) {
        // setattr of something that is not a column persists nothing.
        if (listed(kMeetingColumns, key)) {
            (*row)[key] = value;
        }
    }
}

void FakeMeetingRepository::rename_meeting(std::string_view meeting_id, std::string_view title) {
    const std::string clean = normalize_title(title);
    std::lock_guard lock(mutex_);
    Json* row = meeting_row(meeting_id);
    if (row == nullptr) {
        throw py::ValueError("unknown meeting '" + std::string(meeting_id) + "'");
    }
    Json state = parse_state_json(row->at("state_json")).value_or(Json::object());
    state["meeting_id"] = meeting_id;
    state["title"] = clean;
    (*row)["title"] = clean;
    (*row)["state_json"] = py::dumps(state, {.ensure_ascii = false});
}

void FakeMeetingRepository::replace_tokens(std::string_view meeting_id, std::string_view host_token,
                                           std::string_view guest_token) {
    std::lock_guard lock(mutex_);
    Json* row = meeting_row(meeting_id);
    if (row == nullptr) {
        throw py::ValueError("unknown meeting '" + std::string(meeting_id) + "'");
    }
    (*row)["host_token"] = host_token;
    (*row)["guest_token"] = guest_token;
}

std::optional<Json> FakeMeetingRepository::get_meeting(std::string_view meeting_id) {
    std::lock_guard lock(mutex_);
    const Json* row = meeting_row(meeting_id);
    return row ? std::optional<Json>(*row) : std::nullopt;
}

std::vector<Json> FakeMeetingRepository::list_meetings() {
    std::lock_guard lock(mutex_);
    std::vector<Json> rows = meetings_;
    std::stable_sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
        return text_of(a.at("started_at")) > text_of(b.at("started_at"));
    });
    return rows;
}

void FakeMeetingRepository::delete_meeting(std::string_view meeting_id) {
    std::lock_guard lock(mutex_);
    std::erase_if(events_, [&](const Event& e) { return e.meeting_id == meeting_id; });
    items_.erase(std::string(meeting_id));
    questions_.erase(std::string(meeting_id));
    participants_.erase(std::string(meeting_id));
    std::erase_if(segments_,
                  [&](const SegmentRow& row) { return is(row.row.at("meeting_id"), meeting_id); });
    std::erase_if(deliveries_, [&](const Delivery& d) { return d.meeting_id == meeting_id; });
    std::erase_if(chunks_, [&](const Json& row) { return is(row.at("meeting_id"), meeting_id); });
    std::erase_if(cursors_, [&](const auto& entry) { return entry.first.first == meeting_id; });
    std::erase_if(meetings_, [&](const Json& row) { return is(row.at("id"), meeting_id); });
}

void FakeMeetingRepository::heartbeat(std::string_view meeting_id, std::int64_t pid,
                                      std::string_view utc_iso) {
    Json fields = Json::object();
    fields["app_pid"] = pid;
    fields["app_heartbeat_at"] = utc_iso;
    update_meeting(meeting_id, fields);
}

std::vector<Json> FakeMeetingRepository::find_interrupted_meetings() {
    std::lock_guard lock(mutex_);
    std::set<std::string> unfinished;
    for (const auto& chunk : chunks_) {
        if (!is(chunk.at("asr_status"), "done")) {
            unfinished.insert(text_of(chunk.at("meeting_id")));
        }
    }
    std::vector<Json> rows;
    for (const auto& row : meetings_) {
        const Json& status = row.at("status");
        if (is(status, "active") || is(status, "paused") || is(status, "ending") ||
            is(status, "needs_recovery") || unfinished.contains(text_of(row.at("id")))) {
            rows.push_back(row);
        }
    }
    return rows;
}

// Chunks

std::int64_t FakeMeetingRepository::register_chunk(const Json& fields) {
    std::lock_guard lock(mutex_);
    return insert_chunk(fields);
}

std::int64_t FakeMeetingRepository::insert_chunk(const Json& fields) {
    if (register_failures_ > 0) {
        --register_failures_;
        throw std::runtime_error("database is locked");
    }
    Json row = Json::object();
    for (const auto column : kChunkColumns) {
        row[std::string(column)] = nullptr;
    }
    row["asr_status"] = "pending";
    row["asr_attempts"] = 0;
    for (const auto& [key, value] : fields.items()) {
        if (!listed(kChunkColumns, key)) {
            throw py::TypeError("'" + key +
                                "' is an invalid keyword argument for MeetingAudioChunk");
        }
        row[key] = value;
    }
    for (const auto column : kChunkRequired) {
        if (row.at(std::string(column)).is_null()) {
            throw std::runtime_error("NOT NULL constraint failed: meeting_audio_chunks." +
                                     std::string(column));
        }
    }
    for (const auto& chunk : chunks_) {
        if (chunk.at("meeting_id") == row.at("meeting_id") &&
            chunk.at("channel") == row.at("channel") && chunk.at("seq") == row.at("seq")) {
            throw std::runtime_error("UNIQUE constraint failed: meeting_audio_chunks.meeting_id, "
                                     "meeting_audio_chunks.channel, meeting_audio_chunks.seq");
        }
    }
    if (row.at("id").is_null()) {
        row["id"] = next_chunk_id_;
    }
    next_chunk_id_ = std::max(next_chunk_id_, row.at("id").get<std::int64_t>() + 1);
    const std::int64_t id = row.at("id").get<std::int64_t>();
    chunks_.push_back(std::move(row));
    return id;
}

ChunkRegistration FakeMeetingRepository::register_chunk_if_missing(const Json& fields) {
    // One critical section, as Python's single session: two recoveries racing for the same
    // (meeting, channel, seq) get one row, not a UNIQUE failure.
    std::lock_guard lock(mutex_);
    const std::string meeting_id = text_of(detail::at(fields, "meeting_id"));
    const std::string channel = text_of(detail::at(fields, "channel"));
    const std::int64_t seq = detail::py_int_or_throw(detail::at(fields, "seq"));
    for (const auto& chunk : chunks_) {
        if (is(chunk.at("meeting_id"), meeting_id) && is(chunk.at("channel"), channel) &&
            integer_of(chunk.at("seq")) == seq) {
            return {chunk.at("id").get<std::int64_t>(), false};
        }
    }
    return {insert_chunk(fields), true};
}

void FakeMeetingRepository::set_chunk_status(std::int64_t chunk_id, std::string_view status,
                                             std::optional<std::string_view> error) {
    std::lock_guard lock(mutex_);
    Json* row = chunk_row(chunk_id);
    if (row == nullptr) {
        return;
    }
    if (is(row->at("asr_status"), "done") && status != "done") {
        return;
    }
    (*row)["asr_status"] = status;
    if (status == "processing") {
        (*row)["asr_attempts"] = integer_of(row->at("asr_attempts")) + 1;
    }
    if (error) {
        (*row)["asr_error"] = error_text(*error);
    }
}

std::vector<Json>
FakeMeetingRepository::get_pending_chunks(std::string_view meeting_id,
                                          std::optional<std::int64_t> limit,
                                          const std::vector<std::int64_t>& exclude_ids) {
    std::lock_guard lock(mutex_);
    if (limit && *limit <= 0) {
        return {};
    }
    std::vector<Json> rows;
    for (const auto& chunk : chunks_) {
        const Json& status = chunk.at("asr_status");
        const std::int64_t id = chunk.at("id").get<std::int64_t>();
        if (is(chunk.at("meeting_id"), meeting_id) &&
            (is(status, "pending") || is(status, "failed") || is(status, "processing")) &&
            integer_of(chunk.at("asr_attempts")) < MAX_CHUNK_ATTEMPTS &&
            std::find(exclude_ids.begin(), exclude_ids.end(), id) == exclude_ids.end()) {
            rows.push_back(chunk);
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
        const double sa = number_of(a.at("start_s"));
        const double sb = number_of(b.at("start_s"));
        if (sa != sb) {
            return sa < sb;
        }
        return a.at("id").get<std::int64_t>() < b.at("id").get<std::int64_t>();
    });
    if (limit && rows.size() > static_cast<std::size_t>(*limit)) {
        rows.resize(static_cast<std::size_t>(*limit));
    }
    return rows;
}

std::int64_t FakeMeetingRepository::count_unfinished_chunks(std::string_view meeting_id) {
    std::lock_guard lock(mutex_);
    return std::count_if(chunks_.begin(), chunks_.end(), [&](const Json& chunk) {
        return is(chunk.at("meeting_id"), meeting_id) && !is(chunk.at("asr_status"), "done");
    });
}

std::int64_t FakeMeetingRepository::reset_unfinished_chunks(std::string_view meeting_id,
                                                            bool force) {
    std::lock_guard lock(mutex_);
    std::int64_t reset = 0;
    for (auto& chunk : chunks_) {
        if (!is(chunk.at("meeting_id"), meeting_id) || is(chunk.at("asr_status"), "done")) {
            continue;
        }
        // A startup recovery must not hot-loop exhausted or invalid chunks on every launch.
        if (!force && (is(chunk.at("asr_status"), "blocked") ||
                       integer_of(chunk.at("asr_attempts")) >= MAX_CHUNK_ATTEMPTS)) {
            continue;
        }
        chunk["asr_status"] = "pending";
        if (force) {
            chunk["asr_attempts"] = 0;
            chunk["asr_error"] = nullptr;
        }
        ++reset;
    }
    return reset;
}

void FakeMeetingRepository::defer_chunk_after_connection_failure(std::int64_t chunk_id,
                                                                 std::string_view error) {
    std::lock_guard lock(mutex_);
    Json* row = chunk_row(chunk_id);
    if (row == nullptr || is(row->at("asr_status"), "done")) {
        return;
    }
    (*row)["asr_status"] = "pending";
    (*row)["asr_attempts"] = std::max<std::int64_t>(0, integer_of(row->at("asr_attempts")) - 1);
    (*row)["asr_error"] = error_text(error);
}

std::vector<Json> FakeMeetingRepository::get_audio_chunks(std::string_view meeting_id) {
    std::lock_guard lock(mutex_);
    std::vector<Json> rows;
    for (const auto& chunk : chunks_) {
        if (is(chunk.at("meeting_id"), meeting_id)) {
            rows.push_back(chunk);
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
        const double sa = number_of(a.at("start_s"));
        const double sb = number_of(b.at("start_s"));
        if (sa != sb) {
            return sa < sb;
        }
        const std::string ca = text_of(a.at("channel"));
        const std::string cb = text_of(b.at("channel"));
        if (ca != cb) {
            return ca < cb;
        }
        return integer_of(a.at("seq")) < integer_of(b.at("seq"));
    });
    return rows;
}

std::int64_t FakeMeetingRepository::next_chunk_seq(std::string_view meeting_id,
                                                   std::string_view channel) {
    std::lock_guard lock(mutex_);
    std::optional<std::int64_t> highest;
    for (const auto& chunk : chunks_) {
        if (is(chunk.at("meeting_id"), meeting_id) && is(chunk.at("channel"), channel)) {
            const std::int64_t seq = integer_of(chunk.at("seq"));
            highest = highest ? std::max(*highest, seq) : seq;
        }
    }
    return highest ? *highest + 1 : 0;
}

std::int64_t FakeMeetingRepository::mark_chunks_done(std::string_view meeting_id) {
    std::lock_guard lock(mutex_);
    std::int64_t changed = 0;
    for (auto& chunk : chunks_) {
        if (is(chunk.at("meeting_id"), meeting_id) && !is(chunk.at("asr_status"), "done")) {
            chunk["asr_status"] = "done";
            chunk["asr_error"] = nullptr;
            ++changed;
        }
    }
    return changed;
}

// Segments

void FakeMeetingRepository::add_segments(const std::vector<TranscriptSegment>& segments) {
    if (segments.empty()) {
        return;
    }
    const std::string created = utc_now_iso(clock_);
    std::lock_guard lock(mutex_);
    std::set<std::string> seen;
    for (const auto& segment : segments) {
        if (segment_row(segment.segment_id) != nullptr || !seen.insert(segment.segment_id).second) {
            throw std::runtime_error("UNIQUE constraint failed: meeting_segments.id");
        }
    }
    for (const auto& segment : segments) {
        insert_segment(segment, created);
    }
}

ChunkCommit
FakeMeetingRepository::commit_chunk_transcription(std::string_view meeting_id,
                                                  std::int64_t chunk_id,
                                                  const std::vector<TranscriptSegment>& segments) {
    const std::string created = utc_now_iso(clock_);
    std::lock_guard lock(mutex_);
    Json* chunk = chunk_row(chunk_id);
    if (chunk == nullptr || !is(chunk->at("meeting_id"), meeting_id)) {
        throw py::ValueError("unknown meeting audio chunk");
    }
    const auto chunk_rows = [&] {
        std::vector<Json> rows;
        for (const SegmentRow* row : meeting_segments(meeting_id)) {
            const Json& owner = row->row.at("chunk_id");
            if (owner.is_number_integer() && owner.get<std::int64_t>() == chunk_id) {
                rows.push_back(segment_dict(*row));
            }
        }
        return rows;
    };
    if (is(chunk->at("asr_status"), "done")) {
        return {chunk_rows(), false};
    }
    for (const auto& segment : segments) {
        if (segment.meeting_id != meeting_id || segment.chunk_id != chunk_id) {
            throw py::ValueError("segment does not belong to chunk");
        }
        const SegmentRow* existing = segment_row(segment.segment_id);
        if (existing != nullptr && (!is(existing->row.at("meeting_id"), meeting_id) ||
                                    existing->row.at("chunk_id") != Json(chunk_id))) {
            throw py::ValueError("segment id already belongs to another chunk");
        }
    }
    for (const auto& segment : segments) {
        // merge(): an existing row is updated in place and gets no new delivery sequence.
        if (SegmentRow* existing = segment_row(segment.segment_id)) {
            existing->row = segment_columns(segment, created);
            existing->embedding = segment.embedding;
        } else {
            insert_segment(segment, created);
        }
    }
    (*chunk)["asr_status"] = "done";
    (*chunk)["asr_error"] = nullptr;
    return {chunk_rows(), true};
}

std::optional<Json> FakeMeetingRepository::get_segment(std::string_view meeting_id,
                                                       std::string_view segment_id) {
    std::lock_guard lock(mutex_);
    const SegmentRow* row = segment_row(segment_id);
    if (row == nullptr || !is(row->row.at("meeting_id"), meeting_id)) {
        return std::nullopt;
    }
    return segment_dict(*row);
}

bool FakeMeetingRepository::segment_exists(std::string_view meeting_id,
                                           std::string_view segment_id) {
    std::lock_guard lock(mutex_);
    const SegmentRow* row = segment_row(segment_id);
    return row != nullptr && is(row->row.at("meeting_id"), meeting_id);
}

std::map<std::string, bool, std::less<>>
FakeMeetingRepository::segment_flags(std::string_view meeting_id,
                                     const std::vector<std::string>& segment_ids) {
    std::lock_guard lock(mutex_);
    std::map<std::string, bool, std::less<>> flags;
    for (const auto& id : segment_ids) {
        if (id.empty()) {
            continue;
        }
        const SegmentRow* row = segment_row(id);
        if (row != nullptr && is(row->row.at("meeting_id"), meeting_id)) {
            flags[id] = truthy(row->row.at("speaker_pinned"));
        }
    }
    return flags;
}

void FakeMeetingRepository::update_segment_speaker(std::string_view meeting_id,
                                                   std::string_view segment_id,
                                                   const std::optional<std::string>& participant_id,
                                                   std::string_view source, bool pinned) {
    std::lock_guard lock(mutex_);
    SegmentRow* row = segment_row(segment_id);
    if (row == nullptr || !is(row->row.at("meeting_id"), meeting_id)) {
        return;
    }
    if (participant_id) {
        const auto people = participants_.find(meeting_id);
        if (people == participants_.end() || !people->second.contains(*participant_id)) {
            throw py::ValueError("participant does not belong to meeting");
        }
    }
    row->row["speaker_participant_id"] = participant_id ? Json(*participant_id) : Json();
    row->row["speaker_source"] = source;
    row->row["speaker_pinned"] = pinned;
}

std::vector<Json> FakeMeetingRepository::get_segments(std::string_view meeting_id,
                                                      double after_start_s,
                                                      std::optional<std::int64_t> limit) {
    std::lock_guard lock(mutex_);
    std::vector<Json> rows;
    for (const SegmentRow* row : meeting_segments(meeting_id)) {
        if (number_of(row->row.at("start_s")) > after_start_s) {
            rows.push_back(segment_dict(*row));
        }
    }
    if (limit && *limit > 0 && rows.size() > static_cast<std::size_t>(*limit)) {
        rows.resize(static_cast<std::size_t>(*limit));
    }
    return rows;
}

std::vector<Json> FakeMeetingRepository::get_segments_page(
    std::string_view meeting_id, std::optional<double> cursor_start_s,
    const std::optional<std::string>& cursor_id, std::int64_t limit) {
    const auto bounded = static_cast<std::size_t>(std::clamp<std::int64_t>(limit, 1, 1001));
    std::lock_guard lock(mutex_);
    std::vector<Json> rows;
    for (const SegmentRow* row : meeting_segments(meeting_id)) {
        if (cursor_start_s && cursor_id) {
            const double start = number_of(row->row.at("start_s"));
            const std::string& id = row->row.at("id").get_ref<const std::string&>();
            if (!(start > *cursor_start_s || (start == *cursor_start_s && id > *cursor_id))) {
                continue;
            }
        }
        rows.push_back(segment_dict(*row));
        if (rows.size() == bounded) {
            break;
        }
    }
    return rows;
}

std::vector<Json> FakeMeetingRepository::get_last_segments(std::string_view meeting_id,
                                                           std::int64_t count) {
    std::lock_guard lock(mutex_);
    const auto ordered = meeting_segments(meeting_id);
    // SQLite reads a negative LIMIT as no limit.
    const std::size_t take =
        count < 0 ? ordered.size() : std::min(ordered.size(), static_cast<std::size_t>(count));
    std::vector<Json> rows;
    for (std::size_t i = ordered.size() - take; i < ordered.size(); ++i) {
        rows.push_back(segment_dict(*ordered[i]));
    }
    return rows;
}

TranscriptReplacement
FakeMeetingRepository::replace_final_transcript(std::string_view meeting_id,
                                                const std::vector<TranscriptSegment>& segments) {
    const std::string created = utc_now_iso(clock_);
    std::lock_guard lock(mutex_);
    std::set<std::string> incoming;
    for (const auto& segment : segments) {
        incoming.insert(segment.segment_id);
    }
    for (const auto& segment : segments) {
        if (segment.meeting_id != meeting_id) {
            throw py::ValueError("offline segment belongs to another meeting");
        }
    }
    TranscriptReplacement result;
    std::vector<std::string> retained;
    for (const SegmentRow* row : meeting_segments(meeting_id)) {
        const std::string id = row->row.at("id").get<std::string>();
        if (incoming.contains(id)) {
            continue;
        }
        if (truthy(row->row.at("speaker_pinned"))) {
            retained.push_back(id);
        } else {
            result.deleted_ids.push_back(id);
        }
    }
    for (const auto& id : result.deleted_ids) {
        erase_segment(id);
    }
    for (const auto& segment : segments) {
        if (SegmentRow* existing = segment_row(segment.segment_id)) {
            const Json kept_created = existing->row.at("created_at");
            existing->row = segment_columns(segment, created);
            existing->row["created_at"] = kept_created;
            existing->embedding = segment.embedding;
        } else {
            insert_segment(segment, created);
        }
    }
    for (const SegmentRow* row : meeting_segments(meeting_id)) {
        result.rows.push_back(segment_dict(*row));
    }
    return result;
}

// Delivery

std::vector<Json> FakeMeetingRepository::get_segments_delivery_page(std::string_view meeting_id,
                                                                    std::int64_t after_seq,
                                                                    std::int64_t limit) {
    const auto bounded = static_cast<std::size_t>(std::clamp<std::int64_t>(limit, 1, 301));
    const std::int64_t after = std::max<std::int64_t>(0, after_seq);
    std::lock_guard lock(mutex_);
    std::vector<Json> rows;
    for (const auto& delivery : deliveries_) {
        if (delivery.meeting_id != meeting_id || delivery.seq <= after) {
            continue;
        }
        const SegmentRow* row = segment_row(delivery.segment_id);
        if (row == nullptr) {
            continue;
        }
        Json dict = segment_dict(*row);
        dict["delivery_seq"] = delivery.seq;
        rows.push_back(std::move(dict));
        if (rows.size() == bounded) {
            break;
        }
    }
    return rows;
}

std::int64_t FakeMeetingRepository::get_delivery_cursor(std::string_view meeting_id,
                                                        std::string_view consumer) {
    if (consumer != "cards" && consumer != "notes") {
        throw py::ValueError("unknown transcript delivery consumer");
    }
    std::lock_guard lock(mutex_);
    const auto it = cursors_.find({std::string(meeting_id), std::string(consumer)});
    return it == cursors_.end() ? 0 : it->second;
}

void FakeMeetingRepository::advance_delivery_cursor(std::string_view meeting_id,
                                                    std::string_view consumer, std::int64_t seq) {
    if ((consumer != "cards" && consumer != "notes") || seq < 0) {
        throw py::ValueError("invalid transcript delivery cursor");
    }
    std::lock_guard lock(mutex_);
    auto& cursor = cursors_[{std::string(meeting_id), std::string(consumer)}];
    cursor = std::max(cursor, seq);
}

std::int64_t FakeMeetingRepository::get_delivery_high_water(std::string_view meeting_id) {
    std::lock_guard lock(mutex_);
    std::int64_t highest = 0;
    for (const auto& delivery : deliveries_) {
        if (delivery.meeting_id == meeting_id) {
            highest = std::max(highest, delivery.seq);
        }
    }
    return highest;
}

// State write-through

void FakeMeetingRepository::mirror_effect(std::string_view meeting_id, OpResult& result) {
    if (!result.effect.is_object()) {
        return;
    }
    Json& effect = result.effect;
    const Json& entity = get(effect, "entity");
    const std::string key(meeting_id);
    if (is(entity, "review")) {
        for (const auto& item : get(effect, "items")) {
            items_[key][text_of(detail::at(item, "id"))] = item;
        }
    } else if (is(entity, "item")) {
        const Json& item = detail::at(effect, "item");
        items_[key][text_of(detail::at(item, "id"))] = item;
    } else if (is(entity, "question")) {
        const Json& question = detail::at(effect, "question");
        questions_[key][text_of(detail::at(question, "id"))] = question;
    } else if (is(entity, "participant")) {
        const Json& participant = detail::at(effect, "participant");
        participants_[key][text_of(detail::at(participant, "id"))] = participant;
    } else if (is(entity, "segment_speaker") || is(entity, "segment_text")) {
        SegmentRow* row = segment_row(text_of(detail::at(effect, "segment_id")));
        if (row == nullptr || !is(row->row.at("meeting_id"), meeting_id)) {
            throw py::ValueError("segment does not belong to meeting");
        }
        if (is(entity, "segment_speaker")) {
            const Json& participant_id = get(effect, "participant_id");
            if (!participant_id.is_null()) {
                const auto people = participants_.find(meeting_id);
                if (people == participants_.end() ||
                    !people->second.contains(text_of(participant_id))) {
                    throw py::ValueError("participant does not belong to meeting");
                }
            }
            row->row["speaker_participant_id"] = participant_id;
            row->row["speaker_source"] =
                detail::has(effect, "source") ? get(effect, "source") : Json("human");
            row->row["speaker_pinned"] = truthy(get(effect, "pinned"));
        } else {
            const Json& raw = get(effect, "text");
            const std::string text = py::strip(truthy(raw) ? text_of(raw) : std::string());
            if (text.empty()) {
                throw py::ValueError("segment text must be non-empty");
            }
            row->row["text"] = text;
            effect["segment"] = segment_dict(*row);
        }
    }
}

Json FakeMeetingRepository::on_ops_applied(std::string_view meeting_id, const Json& state,
                                           std::vector<OpResult>& results,
                                           std::string_view actor_type,
                                           const std::optional<std::string>& actor_id) {
    const std::string ts = utc_now_iso(clock_);
    std::lock_guard lock(mutex_);
    ops_applied_calls_.push_back(
        {std::string(meeting_id), state, results, std::string(actor_type), actor_id});
    if (ops_applied_failure_) {
        throw std::runtime_error(*ops_applied_failure_);
    }
    Json snapshot = state;
    // Scoped corrections are validated against the raw row with the batch's final global
    // rules, before anything is written.
    const TermRules global_rules = term_rules(snapshot);
    std::map<std::string, std::string> revised_raw;
    for (const auto& result : results) {
        if (is(get(result.effect, "entity"), "segment_text")) {
            revised_raw[text_of(get(result.effect, "segment_id"))] =
                text_of(get(result.effect, "text"));
        }
    }
    for (const auto& result : results) {
        const Json& data = get(result.op, "data");
        if (!is(get(result.op, "op"), "add_item") || !is(get(result.op, "card"), "user_notes") ||
            !is(get(truthy(data) ? data : Json::object(), "kind"), "occurrence_correction")) {
            continue;
        }
        const std::string segment_id = text_of(detail::at(result.op, "evidence").at(0));
        std::optional<std::string> raw;
        if (const auto revised = revised_raw.find(segment_id); revised != revised_raw.end()) {
            raw = revised->second;
        } else if (const SegmentRow* row = segment_row(segment_id);
                   row != nullptr && is(row->row.at("meeting_id"), meeting_id)) {
            raw = row->row.at("text").get<std::string>();
        }
        const Json& expected = detail::at(data, "base_fingerprint");
        if (!raw || !is(expected, segment_fingerprint(correct_text(*raw, global_rules)))) {
            throw StaleCorrectionSelection(segment_id);
        }
    }
    // One transaction: a failed mirror leaves nothing written.
    const auto saved_events = events_;
    const auto saved_items = items_;
    const auto saved_questions = questions_;
    const auto saved_participants = participants_;
    const auto saved_segments = segments_;
    try {
        for (auto& result : results) {
            const Json& undo_seq = get(result.op, "_undo_event_seq");
            Json action = detail::has(result.op, "op") ? get(result.op, "op") : Json("unknown");
            if (actor_type == "system" && undo_seq.is_number_integer()) {
                action = "undo:" + std::to_string(undo_seq.get<std::int64_t>());
            }
            Json event = Json::object();
            event["seq"] = result.seq.value_or(0);
            event["ts"] = ts;
            event["actor_type"] = actor_type;
            event["actor_id"] = actor_id ? Json(*actor_id) : Json();
            event["action"] = action;
            event["target_id"] = result.target_id ? Json(*result.target_id) : Json();
            event["payload"] = result.op;
            event["inverse"] = truthy(result.inverse) ? result.inverse : Json();
            events_.push_back({std::string(meeting_id), std::move(event)});
            mirror_effect(meeting_id, result);
        }
    } catch (...) {
        events_ = saved_events;
        items_ = saved_items;
        questions_ = saved_questions;
        participants_ = saved_participants;
        segments_ = saved_segments;
        throw;
    }
    if (Json* row = meeting_row(meeting_id)) {
        for (const auto& result : results) {
            if (result.ok && is(get(result.op, "op"), "set_title")) {
                (*row)["title"] = normalize_title(get(result.op, "text"));
            }
        }
        const Json& title = row->at("title");
        snapshot["title"] = truthy(title) ? title : Json("");
        (*row)["state_json"] = py::dumps(snapshot, {.ensure_ascii = false});
        (*row)["state_seq"] =
            detail::has(snapshot, "seq") ? detail::py_int_or_throw(get(snapshot, "seq")) : 0;
        if (detail::has(snapshot, "cloud_enabled")) {
            (*row)["cloud_enabled"] = truthy(get(snapshot, "cloud_enabled"));
        }
    }
    return snapshot;
}

Json FakeMeetingRepository::persist_state(std::string_view meeting_id, const Json& state) {
    std::lock_guard lock(mutex_);
    if (persist_failure_) {
        throw std::runtime_error(*persist_failure_);
    }
    Json* row = meeting_row(meeting_id);
    if (row == nullptr) {
        throw py::ValueError("unknown meeting '" + std::string(meeting_id) + "'");
    }
    Json snapshot = state;
    const Json& title = row->at("title");
    snapshot["title"] = truthy(title) ? title : Json("");
    (*row)["state_json"] = py::dumps(snapshot, {.ensure_ascii = false});
    if (detail::has(snapshot, "seq")) {
        (*row)["state_seq"] = detail::py_int_or_throw(get(snapshot, "seq"));
    }
    if (detail::has(snapshot, "status")) {
        (*row)["status"] = get(snapshot, "status");
    }
    if (detail::has(snapshot, "cloud_enabled")) {
        (*row)["cloud_enabled"] = truthy(get(snapshot, "cloud_enabled"));
    }
    return snapshot;
}

std::optional<Json> FakeMeetingRepository::get_event(std::string_view meeting_id,
                                                     std::int64_t seq) {
    std::lock_guard lock(mutex_);
    for (const auto& event : events_) {
        if (event.meeting_id == meeting_id && event.row.at("seq").get<std::int64_t>() == seq) {
            return std::optional<Json>(std::in_place, event.row);
        }
    }
    return std::nullopt;
}

bool FakeMeetingRepository::event_is_undone(std::string_view meeting_id, std::int64_t seq) {
    std::lock_guard lock(mutex_);
    const Json action = "undo:" + std::to_string(seq);
    return std::any_of(events_.begin(), events_.end(), [&](const Event& event) {
        return event.meeting_id == meeting_id && event.row.at("action") == action;
    });
}

std::vector<Json> FakeMeetingRepository::list_events(std::string_view meeting_id,
                                                     std::optional<std::int64_t> before_seq,
                                                     std::int64_t limit) {
    const auto bounded = static_cast<std::size_t>(std::clamp<std::int64_t>(limit, 1, 500));
    std::lock_guard lock(mutex_);
    std::set<std::string> undo_actions;
    std::vector<const Json*> rows;
    for (const auto& event : events_) {
        if (event.meeting_id != meeting_id) {
            continue;
        }
        const std::string action = text_of(event.row.at("action"));
        if (action.starts_with("undo:")) {
            undo_actions.insert(action);
        }
        if (!before_seq || event.row.at("seq").get<std::int64_t>() < *before_seq) {
            rows.push_back(&event.row);
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Json* a, const Json* b) {
        return a->at("seq").get<std::int64_t>() > b->at("seq").get<std::int64_t>();
    });
    std::vector<Json> out;
    for (const Json* row : rows) {
        if (out.size() == bounded) {
            break;
        }
        Json entry = Json::object();
        for (const char* key :
             {"seq", "ts", "actor_type", "actor_id", "action", "target_id", "payload"}) {
            entry[key] = row->at(key);
        }
        entry["undoable"] =
            !row->at("inverse").is_null() &&
            !undo_actions.contains("undo:" + std::to_string(row->at("seq").get<std::int64_t>()));
        out.push_back(std::move(entry));
    }
    return out;
}

std::vector<Json>
FakeMeetingRepository::search_transcripts(std::string_view query,
                                          const std::optional<std::string>& exclude_meeting_id,
                                          std::int64_t limit) {
    const std::string needle = py::strip(query);
    if (needle.empty()) {
        return {};
    }
    const auto terms = py::split(py::casefold(needle));
    const auto bounded = static_cast<std::size_t>(std::clamp<std::int64_t>(limit, 1, 200));
    const std::string exclude = py::strip(exclude_meeting_id.value_or(""));
    std::lock_guard lock(mutex_);
    std::vector<Json> rows;
    for (const auto& segment : segments_) {
        const Json& row = segment.row;
        const std::string meeting_id = text_of(row.at("meeting_id"));
        if (!exclude.empty() && meeting_id == exclude) {
            continue;
        }
        const std::string folded = py::casefold(text_of(row.at("text")));
        const bool all = std::all_of(terms.begin(), terms.end(), [&](const std::string& term) {
            return folded.find(term) != std::string::npos;
        });
        const Json* meeting = meeting_row(meeting_id);
        if (!all || meeting == nullptr) {
            continue;
        }
        Json hit = Json::object();
        hit["segment_id"] = row.at("id");
        hit["meeting_id"] = row.at("meeting_id");
        hit["start_s"] = row.at("start_s");
        hit["end_s"] = row.at("end_s");
        hit["text"] = row.at("text");
        hit["title"] = meeting->at("title");
        hit["started_at"] = meeting->at("started_at");
        hit["snippet"] = row.at("text");
        rows.push_back(std::move(hit));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
        const std::string sa = text_of(a.at("started_at"));
        const std::string sb = text_of(b.at("started_at"));
        if (sa != sb) {
            return sa > sb;
        }
        return number_of(a.at("start_s")) < number_of(b.at("start_s"));
    });
    if (rows.size() > bounded) {
        rows.resize(bounded);
    }
    return rows;
}

// Test hooks

void FakeMeetingRepository::fail_register_chunk(int times) {
    std::lock_guard lock(mutex_);
    register_failures_ = times;
}

void FakeMeetingRepository::fail_on_ops_applied(std::optional<std::string> message) {
    std::lock_guard lock(mutex_);
    ops_applied_failure_ = std::move(message);
}

void FakeMeetingRepository::fail_persist_state(std::optional<std::string> message) {
    std::lock_guard lock(mutex_);
    persist_failure_ = std::move(message);
}

std::vector<FakeMeetingRepository::OpsAppliedCall>
FakeMeetingRepository::ops_applied_calls() const {
    std::lock_guard lock(mutex_);
    return ops_applied_calls_;
}

namespace {

template <class Map>
std::optional<Json> lookup(const Map& map, std::string_view meeting_id, std::string_view id) {
    const auto meeting = map.find(meeting_id);
    if (meeting == map.end()) {
        return std::nullopt;
    }
    const auto found = meeting->second.find(id);
    return found == meeting->second.end() ? std::nullopt : std::optional<Json>(found->second);
}

} // namespace

std::optional<Json> FakeMeetingRepository::mirrored_item(std::string_view meeting_id,
                                                         std::string_view item_id) const {
    std::lock_guard lock(mutex_);
    return lookup(items_, meeting_id, item_id);
}

std::optional<Json> FakeMeetingRepository::mirrored_question(std::string_view meeting_id,
                                                             std::string_view question_id) const {
    std::lock_guard lock(mutex_);
    return lookup(questions_, meeting_id, question_id);
}

std::optional<Json>
FakeMeetingRepository::mirrored_participant(std::string_view meeting_id,
                                            std::string_view participant_id) const {
    std::lock_guard lock(mutex_);
    return lookup(participants_, meeting_id, participant_id);
}

std::vector<Json> FakeMeetingRepository::events(std::string_view meeting_id) const {
    std::lock_guard lock(mutex_);
    std::vector<Json> rows;
    for (const auto& event : events_) {
        if (event.meeting_id == meeting_id) {
            rows.push_back(event.row);
        }
    }
    return rows;
}

} // namespace fv::meeting_state::testing
