#include "meeting_state/sql_repository.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include "base/log.h"
#include "db/database.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/sql_internal.h"
#include "meeting_state/schema.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger corrections_logger("meeting.corrections");

// The cache of one provider: the state_seq its rules were read at (nullopt before the first
// read, Python's `unseen`).
struct TermRulesCache {
    std::mutex mutex;
    std::optional<Json> seq;
    TermRules rules;
};

TermRules provide_rules(MeetingRepository* repository, const std::string& meeting_id,
                        TermRulesCache& cache) {
    if (repository == nullptr) {
        return {};
    }
    std::lock_guard lock(cache.mutex);
    try {
        const std::optional<Json> meeting = repository->get_meeting(meeting_id);
        if (!meeting || !meeting->is_object()) {
            return {};
        }
        const Json& seq = detail::get(*meeting, "state_seq");
        if (!cache.seq || *cache.seq != seq) {
            cache.rules = term_rules(
                parse_state_json(detail::get(*meeting, "state_json")).value_or(Json::object()));
            cache.seq = seq;
        }
    } catch (const std::exception& error) {
        corrections_logger.error_with(error, "Could not load term corrections for " + meeting_id);
        return {};
    }
    return cache.rules;
}

} // namespace

double interval_iou(double a_start, double a_end, double b_start, double b_end) {
    const double start = std::max(a_start, b_start);
    const double end = std::min(a_end, b_end);
    const double inter = std::max(0.0, end - start);
    if (inter <= 0.0) {
        return 0.0;
    }
    const double union_s = std::max(a_end, b_end) - std::min(a_start, b_start);
    if (union_s <= 0.0) {
        return 0.0;
    }
    return inter / union_s;
}

std::function<TermRules()> repository_term_rules(std::shared_ptr<MeetingRepository> repository,
                                                 std::string meeting_id) {
    auto cache = std::make_shared<TermRulesCache>();
    return [repository = std::move(repository), meeting_id = std::move(meeting_id), cache] {
        return provide_rules(repository.get(), meeting_id, *cache);
    };
}

SqlMeetingRepository::SqlMeetingRepository(Database& database, const Clock& clock)
    : database_(database), clock_(clock) {}

std::function<Json()> SqlMeetingRepository::term_rules_provider(std::string meeting_id) {
    auto cache = std::make_shared<TermRulesCache>();
    return [this, meeting_id = std::move(meeting_id), cache] {
        Json rules = Json::object();
        for (const auto& [term, replacement] : provide_rules(this, meeting_id, *cache)) {
            rules[term] = replacement;
        }
        return rules;
    };
}

void SqlMeetingRepository::set_feature_counter(std::function<void()> count_features) {
    std::lock_guard lock(mutex_);
    count_features_ = std::move(count_features);
}

int SqlMeetingRepository::last_summary_query_count() const {
    std::lock_guard lock(mutex_);
    return last_summary_queries_;
}

namespace detail {

sql::Value to_value(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
        return nullptr;
    case Json::value_t::boolean:
        return std::int64_t{value.get<bool>() ? 1 : 0};
    case Json::value_t::number_integer:
        return value.get<std::int64_t>();
    case Json::value_t::number_unsigned: {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("Python int too large to convert to SQLite INTEGER");
        }
        return static_cast<std::int64_t>(unsigned_value);
    }
    case Json::value_t::number_float:
        return value.get<double>();
    case Json::value_t::string:
        return value.get<std::string>();
    case Json::value_t::array:
    case Json::value_t::object:
    case Json::value_t::binary:
        break;
    }
    throw std::runtime_error("Error binding parameter: type '" +
                             std::string(value.is_array() ? "list" : "dict") +
                             "' is not supported");
}

Json from_value(const sql::Value& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return *integer;
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return *real;
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* blob = std::get_if<sql::Blob>(&value)) {
        Json::binary_t bytes;
        for (const std::byte byte : *blob) {
            bytes.push_back(static_cast<std::uint8_t>(byte));
        }
        return Json::binary(std::move(bytes));
    }
    return nullptr;
}

Json bool_from_value(const sql::Value& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return *integer != 0;
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return *real != 0.0;
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return !text->empty();
    }
    if (const auto* blob = std::get_if<sql::Blob>(&value)) {
        return !blob->empty();
    }
    return nullptr;
}

std::string text_value(const sql::Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return {};
}

std::int64_t int_value(const sql::Value& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return *integer;
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(*real);
    }
    return 0;
}

double real_value(const sql::Value& value) {
    if (const auto* real = std::get_if<double>(&value)) {
        return *real;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*integer);
    }
    return 0.0;
}

Json session_dict(const sql::Statement& statement, int first) {
    static constexpr std::string_view keys[] = {"id",
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
    Json row = Json::object();
    for (int index = 0; index < kSessionColumnCount; ++index) {
        const sql::Value value = statement.column(first + index);
        const std::string key(keys[index]);
        row[key] = key == "cloud_enabled" ? bool_from_value(value) : from_value(value);
    }
    return row;
}

Json chunk_dict(const sql::Statement& statement, int first) {
    static constexpr std::string_view keys[] = {
        "id",         "meeting_id",  "channel",    "seq",          "file_path", "start_s",
        "duration_s", "sample_rate", "asr_status", "asr_attempts", "asr_error"};
    Json row = Json::object();
    for (int index = 0; index < kChunkColumnCount; ++index) {
        row[std::string(keys[index])] = from_value(statement.column(first + index));
    }
    return row;
}

const std::pair<TermRules, ScopedRules>& SegmentReader::rules_for(const std::string& meeting_id) {
    const auto found = cache_.find(meeting_id);
    if (found != cache_.end()) {
        return found->second;
    }
    // Raw ASR text stays as stored; only this read is corrected.
    sql::Statement statement = connection_.prepare(
        "SELECT meeting_sessions.state_json FROM meeting_sessions WHERE meeting_sessions.id = ?");
    statement.bind(1, meeting_id);
    Json state = Json::object();
    if (statement.step()) {
        state = parse_state_json(from_value(statement.column(0))).value_or(Json::object());
    }
    auto rules = std::make_pair(term_rules(state), occurrence_rules(state));
    return cache_.emplace(meeting_id, std::move(rules)).first->second;
}

Json SegmentReader::dict(const sql::Statement& statement, int first) {
    const std::string id = text_value(statement.column(first));
    const std::string meeting_id = text_value(statement.column(first + 1));
    const Json raw = from_value(statement.column(first + 6));
    const auto& [rules, scoped] = rules_for(meeting_id);
    Json row = Json::object();
    row["id"] = from_value(statement.column(first));
    row["meeting_id"] = from_value(statement.column(first + 1));
    row["chunk_id"] = from_value(statement.column(first + 2));
    row["channel"] = from_value(statement.column(first + 3));
    row["start_s"] = from_value(statement.column(first + 4));
    row["end_s"] = from_value(statement.column(first + 5));
    row["text"] =
        raw.is_string()
            ? Json(correct_segment_text(raw.get_ref<const std::string&>(), id, rules, scoped))
            : raw;
    row["original_text"] = raw;
    row["speaker_participant_id"] = from_value(statement.column(first + 7));
    row["speaker_source"] = from_value(statement.column(first + 8));
    row["speaker_pinned"] = bool_from_value(statement.column(first + 9));
    row["created_at"] = from_value(statement.column(first + 10));
    return row;
}

std::vector<Json> SegmentReader::rows(std::string_view sql, const std::vector<sql::Value>& params) {
    sql::Statement statement = connection_.prepare(sql);
    bind_all(statement, params);
    std::vector<Json> out;
    while (statement.step()) {
        out.push_back(dict(statement));
    }
    return out;
}

std::optional<Json> SegmentReader::by_id(std::string_view segment_id) {
    std::vector<Json> found = rows("SELECT " + std::string(kSegmentColumns) +
                                       " FROM meeting_segments WHERE meeting_segments.id = ?",
                                   {std::string(segment_id)});
    if (found.empty()) {
        return std::nullopt;
    }
    return std::move(found.front());
}

void bind_all(sql::Statement& statement, const std::vector<sql::Value>& params) {
    int index = 1;
    for (const auto& param : params) {
        statement.bind(index++, param);
    }
}

std::string placeholders(std::size_t count) {
    std::string out;
    for (std::size_t index = 0; index < count; ++index) {
        out += index == 0 ? "?" : ", ?";
    }
    return out;
}

sql::Value embedding_value(const std::optional<std::vector<std::uint8_t>>& embedding) {
    if (!embedding) {
        return nullptr;
    }
    sql::Blob blob;
    blob.reserve(embedding->size());
    for (const std::uint8_t byte : *embedding) {
        blob.push_back(static_cast<std::byte>(byte));
    }
    return blob;
}

void merge_segment(sql::Connection& connection, const TranscriptSegment& segment,
                   const std::string& created_at) {
    sql::Statement statement = connection.prepare(
        "INSERT INTO meeting_segments (id, meeting_id, chunk_id, channel, start_s, end_s, text, "
        "speaker_participant_id, speaker_source, speaker_pinned, embedding, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (id) DO UPDATE SET meeting_id = excluded.meeting_id, "
        "chunk_id = excluded.chunk_id, channel = excluded.channel, start_s = excluded.start_s, "
        "end_s = excluded.end_s, text = excluded.text, "
        "speaker_participant_id = excluded.speaker_participant_id, "
        "speaker_source = excluded.speaker_source, speaker_pinned = excluded.speaker_pinned, "
        "embedding = excluded.embedding, created_at = excluded.created_at");
    bind_all(statement,
             {segment.segment_id, segment.meeting_id,
              segment.chunk_id ? sql::Value(*segment.chunk_id) : sql::Value(nullptr),
              segment.channel, segment.start_s, segment.end_s, segment.text,
              segment.speaker_participant_id ? sql::Value(*segment.speaker_participant_id)
                                             : sql::Value(nullptr),
              segment.speaker_source, std::int64_t{segment.speaker_pinned ? 1 : 0},
              embedding_value(segment.embedding), created_at});
    statement.step();
}

} // namespace detail

} // namespace fv::meeting_state
