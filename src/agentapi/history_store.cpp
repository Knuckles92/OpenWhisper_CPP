#include "agentapi/history_store.h"

#include <array>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "agentapi/history_cursor.h"
#include "agentapi/history_models.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "db/schema.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "sql/db.h"

namespace fv::agentapi {

namespace {

using sql::Value;

// store.py CARDS: the cards a client may read; any other card in a snapshot stays private.
constexpr std::array<std::string_view, 7> kCards = {
    "key_points", "decisions", "action_items", "risks", "timeline", "live_notes", "user_notes"};

// A piece of SQL and its positional parameters, in text order.
struct Fragment {
    std::string text;
    std::vector<Value> params;

    Fragment& add(std::string_view sql) {
        text += sql;
        return *this;
    }
    Fragment& add(const Fragment& other) {
        text += other.text;
        params.insert(params.end(), other.params.begin(), other.params.end());
        return *this;
    }
    Fragment& bind(Value value) {
        text += "?";
        params.push_back(std::move(value));
        return *this;
    }
};

Fragment join(const std::vector<Fragment>& parts, std::string_view separator) {
    Fragment out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out.add(separator);
        }
        out.add(parts[i]);
    }
    return out;
}

// _time: SQLite normalizes offsets; legacy naive timestamps are read as UTC. 0.0 is bound as a
// REAL, so an unparseable timestamp sorts last and its cursor key is written as 0.0.
Fragment time_of(std::string_view column) {
    Fragment out;
    out.add("coalesce(julianday(").add(column).add("), ").bind(0.0).add(")");
    return out;
}

// SQLAlchemy's icontains(autoescape=True): '/' is the escape character.
std::string escape_like(std::string_view q) {
    std::string out;
    for (const char c : q) {
        if (c == '/' || c == '%' || c == '_') {
            out += '/';
        }
        out += c;
    }
    return out;
}

// _match: lower() and LIKE fold ASCII letters only, so non-ASCII text matches literally.
Fragment match(std::string_view column, std::string_view q) {
    Fragment out;
    out.add("(lower(").add(column).add(") LIKE '%' || lower(").bind(escape_like(q));
    out.add(") || '%' ESCAPE '/')");
    return out;
}

// _excerpt: 320 characters from 80 before the first (ASCII-folded) occurrence.
Fragment excerpt(const Fragment& content, std::string_view q) {
    Fragment out;
    out.add("substr(").add(content).add(", max(").bind(std::int64_t{1}).add(", instr(lower(");
    out.add(content).add("), lower(").bind(std::string(q)).add(")) - ").bind(std::int64_t{80});
    out.add("), ").bind(std::int64_t{320}).add(")");
    return out;
}

struct Filters {
    bool include_remote = false;
    std::optional<std::string> since;
    std::optional<std::string> before;
    std::optional<std::string> origin_device_id;
};

// _filters
std::vector<Fragment> filter_clauses(std::string_view table, std::string_view timestamp,
                                     const Filters& filters) {
    std::vector<Fragment> clauses;
    const std::string origin = std::string(table) + ".origin_device_id";
    if (!filters.include_remote) {
        clauses.push_back(Fragment{}.add(origin).add(" IS NULL"));
    }
    if (filters.origin_device_id) {
        clauses.push_back(Fragment{}.add(origin).add(" = ").bind(*filters.origin_device_id));
    }
    if (filters.since) {
        clauses.push_back(
            Fragment{}.add(time_of(timestamp)).add(" >= julianday(").bind(*filters.since).add(")"));
    }
    if (filters.before) {
        clauses.push_back(
            Fragment{}.add(time_of(timestamp)).add(" < julianday(").bind(*filters.before).add(")"));
    }
    return clauses;
}

// The filters dict of a page's scope fingerprint: the keyword arguments the store received.
Json filters_json(const Filters& filters) {
    Json out = Json::object();
    out["include_remote"] = filters.include_remote;
    out["since"] = filters.since ? Json(*filters.since) : Json(nullptr);
    out["before"] = filters.before ? Json(*filters.before) : Json(nullptr);
    if (filters.origin_device_id) {
        out["origin_device_id"] = *filters.origin_device_id;
    }
    return out;
}

Json optional_json(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

using Row = std::vector<Value>;

std::vector<Row> fetch(sql::Connection& connection, const Fragment& query) {
    sql::Statement statement = connection.prepare(query.text);
    for (std::size_t i = 0; i < query.params.size(); ++i) {
        statement.bind(static_cast<int>(i) + 1, query.params[i]);
    }
    std::vector<Row> rows;
    while (statement.step()) {
        Row row;
        const int count = statement.column_count();
        row.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            row.push_back(statement.column(i));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// _one
Row one(sql::Connection& connection, const Fragment& query) {
    std::vector<Row> rows = fetch(connection, query);
    if (rows.empty()) {
        throw NotFound();
    }
    return std::move(rows.front());
}

// Row values as the models' field types; a value pydantic would refuse is a ModelError.
std::string text(const Value& value) {
    if (const auto* s = std::get_if<std::string>(&value)) {
        return *s;
    }
    throw ModelError("expected text");
}

std::optional<std::string> optional_text(const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        return std::nullopt;
    }
    return text(value);
}

double real(const Value& value) {
    if (const auto* d = std::get_if<double>(&value)) {
        return *d;
    }
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*i);
    }
    throw ModelError("expected a number");
}

std::optional<double> optional_real(const Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        return std::nullopt;
    }
    return real(value);
}

std::int64_t integer(const Value& value) {
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i;
    }
    if (const auto* d = std::get_if<double>(&value)) {
        const auto truncated = static_cast<std::int64_t>(*d);
        if (static_cast<double>(truncated) == *d) {
            return truncated;
        }
    }
    throw ModelError("expected an integer");
}

Json key_json(const Value& value) {
    if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i;
    }
    if (const auto* d = std::get_if<double>(&value)) {
        return *d;
    }
    if (const auto* s = std::get_if<std::string>(&value)) {
        return *s;
    }
    return nullptr;
}

Value key_value(const Json& value) {
    if (value.is_number_integer() && !value.is_number_unsigned()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(unsigned_value);
        }
        return static_cast<double>(unsigned_value);
    }
    if (value.is_number()) {
        return value.get<double>();
    }
    return value.get<std::string>();
}

// urllib.parse.quote(text, safe=""): UTF-8 bytes, letters, digits and "_.-~" kept.
std::string quote(std::string_view text) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '.' || byte == '-' ||
            byte == '~') {
            out += c;
        } else {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 0x0f];
        }
    }
    return out;
}

struct PageSpec {
    std::vector<std::string> columns; // of the inner query, as the outer query selects them
    std::vector<std::string> keys;
    std::vector<bool> descending;
    Json scope;
    std::vector<Json> compatible_scopes;
};

// _page: limit + 1 rows after the cursor's keys, in the keys' order.
template <class Transform>
Json page(const std::filesystem::path& path, const Fragment& inner, const PageSpec& spec,
          std::int64_t limit, const std::optional<std::string>& cursor, Transform&& transform) {
    if (limit < 1 || limit > 100) {
        throw InvalidQuery("limit must be between 1 and 100.");
    }
    const std::string own = fingerprint(spec.scope);
    std::vector<std::string> valid{own};
    for (const Json& previous : spec.compatible_scopes) {
        valid.push_back(fingerprint(previous));
    }
    std::vector<std::size_t> key_index;
    for (const auto& key : spec.keys) {
        for (std::size_t i = 0; i < spec.columns.size(); ++i) {
            if (spec.columns[i] == key) {
                key_index.push_back(i);
            }
        }
    }

    Fragment query;
    query.add("SELECT ");
    for (std::size_t i = 0; i < spec.columns.size(); ++i) {
        query.add(i == 0 ? "" : ", ").add("anon_1.").add(spec.columns[i]);
    }
    query.add(" \nFROM (").add(inner).add(") AS anon_1");
    if (cursor && !cursor->empty()) {
        const Json values = cursor_keys(*cursor, valid, spec.keys.size());
        // _after: rows that sort after `values` in the keys' order.
        std::vector<Fragment> terms;
        for (std::size_t i = 0; i < spec.keys.size(); ++i) {
            Fragment term;
            for (std::size_t j = 0; j < i; ++j) {
                term.add("anon_1.").add(spec.keys[j]).add(" = ").bind(key_value(values[j]));
                term.add(" AND ");
            }
            term.add("anon_1.").add(spec.keys[i]).add(spec.descending[i] ? " < " : " > ");
            term.bind(key_value(values[i]));
            terms.push_back(std::move(term));
        }
        query.add(" \nWHERE ").add(join(terms, " OR "));
    }
    query.add(" ORDER BY ");
    for (std::size_t i = 0; i < spec.keys.size(); ++i) {
        query.add(i == 0 ? "" : ", ").add("anon_1.").add(spec.keys[i]);
        query.add(spec.descending[i] ? " DESC" : " ASC");
    }
    query.add("\n LIMIT ").bind(limit + 1).add(" OFFSET ").bind(std::int64_t{0});

    std::vector<Row> rows;
    {
        sql::Connection connection = open_read_only(path);
        rows = fetch(connection, query);
    }
    std::optional<std::string> next_cursor;
    if (rows.size() > static_cast<std::size_t>(limit)) {
        rows.resize(static_cast<std::size_t>(limit));
        Json keys = Json::array();
        for (const std::size_t index : key_index) {
            keys.push_back(key_json(rows.back()[index]));
        }
        next_cursor = encode_cursor(own, keys);
    }
    Json items = Json::array();
    for (Row& row : rows) {
        items.push_back(transform(row));
    }
    return page_json(std::move(items), next_cursor);
}

constexpr std::string_view kTranscriptionColumns =
    "transcription_history.id AS id, transcription_history.title AS title, "
    "transcription_history.timestamp AS timestamp, transcription_history.model AS model, "
    "transcription_history.source_name AS source_name, transcription_history.audio_duration AS "
    "audio_duration, transcription_history.origin_device_id AS origin_device_id, "
    "transcription_history.origin_device_name AS origin_device_name, ";

constexpr std::string_view kMeetingColumns =
    "meeting_sessions.id AS id, meeting_sessions.title AS title, meeting_sessions.status AS "
    "status, meeting_sessions.started_at AS started_at, meeting_sessions.ended_at AS ended_at, "
    "meeting_sessions.state_seq AS state_seq, meeting_sessions.origin_device_id AS "
    "origin_device_id, meeting_sessions.origin_device_name AS origin_device_name, ";

constexpr std::string_view kMeetingDetailColumns =
    "meeting_sessions.id, meeting_sessions.title, meeting_sessions.status, "
    "meeting_sessions.started_at, meeting_sessions.ended_at, meeting_sessions.state_seq, "
    "meeting_sessions.origin_device_id, meeting_sessions.origin_device_name";

constexpr std::string_view kSegmentColumns =
    "meeting_segments.id, meeting_segments.meeting_id, meeting_segments.start_s, "
    "meeting_segments.end_s, meeting_segments.channel, meeting_segments.text, "
    "meeting_segments.speaker_participant_id, meeting_segments.speaker_source";

Fragment preview_column() {
    Fragment out;
    out.add("substr(transcription_history.text, ").bind(std::int64_t{1}).add(", ");
    out.bind(std::int64_t{320}).add(") AS preview");
    return out;
}

Fragment where(std::vector<Fragment> clauses) {
    Fragment out;
    if (!clauses.empty()) {
        out.add(" \nWHERE ").add(join(clauses, " AND "));
    }
    return out;
}

Json meeting_from(const Row& row) {
    Meeting meeting;
    meeting.id = text(row[0]);
    meeting.title = text(row[1]);
    meeting.status = text(row[2]);
    meeting.started_at = text(row[3]);
    meeting.ended_at = optional_text(row[4]);
    meeting.state_seq = integer(row[5]);
    meeting.origin_device_id = optional_text(row[6]);
    meeting.origin_device_name = optional_text(row[7]);
    return meeting.to_json();
}

Json segment_from(const Row& row) {
    Segment segment;
    segment.id = text(row[0]);
    segment.meeting_id = text(row[1]);
    segment.start_s = real(row[2]);
    segment.end_s = real(row[3]);
    segment.channel = text(row[4]);
    segment.text = text(row[5]);
    segment.speaker_participant_id = optional_text(row[6]);
    segment.speaker_source = text(row[7]);
    return segment.to_json();
}

Fragment meeting_detail(std::string_view id, bool include_remote) {
    Fragment query;
    query.add("SELECT ").add(kMeetingDetailColumns).add(" \nFROM meeting_sessions");
    std::vector<Fragment> clauses;
    clauses.push_back(Fragment{}.add("meeting_sessions.id = ").bind(std::string(id)));
    for (Fragment& clause :
         filter_clauses("meeting_sessions", "meeting_sessions.started_at", {include_remote})) {
        clauses.push_back(std::move(clause));
    }
    return query.add(where(std::move(clauses)));
}

// Python iterating a snapshot value: a list gives its items, an empty dict or string gives none,
// and anything else fails (a dict's keys and a string's characters are not records).
const Json& records(const Json& value) {
    static const Json empty = Json::array();
    if (value.is_array()) {
        return value;
    }
    if ((value.is_object() || value.is_string()) && value.empty()) {
        return empty;
    }
    if (value.is_string() && value.get<std::string>().empty()) {
        return empty;
    }
    throw ModelError("not a list of records");
}

const Json* member(const Json& object, std::string_view name) {
    const auto it = object.find(std::string(name));
    return it == object.end() ? nullptr : &*it;
}

MeetingInsights snapshot_insights(const Json& state, MeetingInsights base) {
    if (!state.is_object()) {
        throw ModelError("the snapshot is not an object");
    }
    if (const Json* summary = member(state, "rolling_summary")) {
        if (!summary->is_string()) {
            throw ModelError("rolling_summary");
        }
        base.rolling_summary = summary->get<std::string>();
    }
    if (const Json* evidence = member(state, "rolling_summary_evidence")) {
        if (!evidence->is_array()) {
            throw ModelError("rolling_summary_evidence");
        }
        for (const Json& item : *evidence) {
            if (!item.is_string()) {
                throw ModelError("rolling_summary_evidence");
            }
            base.rolling_summary_evidence.push_back(item.get<std::string>());
        }
    }
    if (const Json* participants = member(state, "participants")) {
        if (!participants->is_object()) {
            throw ModelError("participants");
        }
        for (const auto& [id, participant] : participants->items()) {
            (void)id;
            base.participants.push_back(participant_from(participant));
        }
    }
    if (const Json* cards = member(state, "cards")) {
        if (!cards->is_object()) {
            throw ModelError("cards");
        }
        for (const auto& [card, items] : cards->items()) {
            bool listed = false;
            for (const std::string_view name : kCards) {
                listed = listed || card == name;
            }
            if (!listed) {
                continue;
            }
            for (const Json& item : records(items)) {
                if (!item.is_object()) {
                    throw ModelError("card item");
                }
                const Json* status = member(item, "status");
                if (status != nullptr && status->is_string() &&
                    status->get<std::string>() == "removed") {
                    continue;
                }
                Json with_card = item;
                with_card["card"] = card;
                base.cards.push_back(insight_from(with_card));
            }
        }
    }
    if (const Json* questions = member(state, "questions")) {
        for (const Json& question : records(*questions)) {
            if (!question.is_object()) {
                throw ModelError("question");
            }
            const Json* status = member(question, "status");
            if (status != nullptr && status->is_string() &&
                status->get<std::string>() == "dismissed") {
                continue;
            }
            base.questions.push_back(question_from(question));
        }
    }
    if (const Json* reports = member(state, "custom_reports")) {
        for (const Json& report : records(*reports)) {
            base.custom_reports.push_back(report_from(report));
        }
    }
    return base;
}

std::string product_sentence(std::string_view before, std::string_view after) {
    return std::string(before) + std::string(identity().product) + std::string(after);
}

} // namespace

sql::Connection open_read_only(const std::filesystem::path& database) {
    sql::Connection connection = sql::Connection::open(database, sql::OpenMode::ReadOnly);
    connection.exec("PRAGMA query_only=ON");
    return connection;
}

HistoryStore::HistoryStore(std::filesystem::path database)
    : HistoryStore(std::move(database), system_env()) {}

HistoryStore::HistoryStore(std::filesystem::path database, const EnvReader& env) {
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(expand_user(database, env), error);
    if (!error) {
        std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
        if (!error) {
            path = std::move(resolved);
        }
    }
    if (!std::filesystem::is_regular_file(path, error)) {
        throw std::invalid_argument(
            product_sentence("Database does not exist. Open ", " first or select --database."));
    }
    path_ = std::move(path);
    sql::Connection connection = open_read_only(path_);
    // Never run migrations from a second process against a live app.
    const Value version = connection.scalar("SELECT version FROM schema_version");
    const bool current = (std::holds_alternative<std::int64_t>(version) &&
                          std::get<std::int64_t>(version) == schema::schema_version) ||
                         (std::holds_alternative<double>(version) &&
                          std::get<double>(version) == static_cast<double>(schema::schema_version));
    if (!current) {
        throw std::invalid_argument(product_sentence(
            "Database schema is incompatible. Open it with this version of ", " first."));
    }
    const auto probe = [&connection](std::string_view table, const auto& columns) {
        Fragment query;
        query.add("SELECT ");
        bool first = true;
        for (const std::string_view column : columns) {
            query.add(first ? "" : ", ").add(table).add(".").add(column);
            first = false;
        }
        query.add(" \nFROM ").add(table).add("\n LIMIT ").bind(std::int64_t{0});
        query.add(" OFFSET ").bind(std::int64_t{0});
        fetch(connection, query);
    };
    probe(schema::transcription_history::table, schema::transcription_history::columns);
    {
        namespace m = schema::meeting_sessions;
        probe(m::table, std::array{m::id,
                                   m::title,
                                   m::status,
                                   m::started_at,
                                   m::ended_at,
                                   m::paused_total_s,
                                   m::host_token,
                                   m::guest_token,
                                   m::cloud_enabled,
                                   m::asr_model,
                                   m::asr_remote_json,
                                   m::agent_provider,
                                   m::agent_model,
                                   m::agent_endpoint_json,
                                   m::spool_dir,
                                   m::state_json,
                                   m::state_seq,
                                   m::app_pid,
                                   m::app_heartbeat_at,
                                   m::origin_device_id,
                                   m::origin_device_name});
    }
    {
        namespace s = schema::meeting_segments;
        probe(s::table, std::array{s::id, s::meeting_id, s::chunk_id, s::channel, s::start_s,
                                   s::end_s, s::text, s::speaker_participant_id, s::speaker_source,
                                   s::speaker_pinned, s::embedding, s::created_at});
    }
}

void HistoryStore::check() {
    sql::Connection connection = open_read_only(path_);
    Fragment query;
    query.add("SELECT transcription_history.id \nFROM transcription_history\n LIMIT ");
    query.bind(std::int64_t{1}).add(" OFFSET ").bind(std::int64_t{0});
    fetch(connection, query);
}

Json HistoryStore::transcriptions(const ListQuery& request) {
    const Filters filters{request.include_remote, request.since, request.before,
                          request.origin_device_id};
    Fragment inner;
    inner.add("SELECT ").add(kTranscriptionColumns).add(preview_column()).add(", ");
    inner.add(time_of("transcription_history.timestamp")).add(" AS sort_time \nFROM ");
    inner.add("transcription_history");
    std::vector<Fragment> clauses =
        filter_clauses("transcription_history", "transcription_history.timestamp", filters);
    if (request.q && !request.q->empty()) {
        std::vector<Fragment> matches;
        for (const std::string_view field : {"text", "raw_text", "source_name", "title"}) {
            matches.push_back(match("transcription_history." + std::string(field), *request.q));
        }
        clauses.push_back(Fragment{}.add("(").add(join(matches, " OR ")).add(")"));
    }
    inner.add(where(std::move(clauses)));

    PageSpec spec;
    spec.columns = {"id",          "title",          "timestamp",        "model",
                    "source_name", "audio_duration", "origin_device_id", "origin_device_name",
                    "preview",     "sort_time"};
    spec.keys = {"sort_time", "id"};
    spec.descending = {true, false};
    spec.scope = Json::array({"transcriptions", optional_json(request.q), filters_json(filters)});
    return page(path_, inner, spec, request.limit, request.cursor, [](const Row& row) {
        TranscriptionSummary summary;
        summary.id = text(row[0]);
        summary.title = optional_text(row[1]);
        summary.timestamp = text(row[2]);
        summary.model = text(row[3]);
        summary.source_name = optional_text(row[4]);
        summary.audio_duration = optional_real(row[5]);
        summary.origin_device_id = optional_text(row[6]);
        summary.origin_device_name = optional_text(row[7]);
        summary.preview = text(row[8]);
        return summary.to_json();
    });
}

Json HistoryStore::transcription(std::string_view id, bool include_remote) {
    Fragment query;
    query.add("SELECT transcription_history.id, transcription_history.title, ");
    query.add("transcription_history.timestamp, transcription_history.model, ");
    query.add("transcription_history.source_name, transcription_history.audio_duration, ");
    query.add("transcription_history.origin_device_id, transcription_history.origin_device_name, ");
    query.add("transcription_history.text, transcription_history.raw_text, ");
    query.add("transcription_history.cleanup_provider, transcription_history.cleanup_model, ");
    query.add(preview_column()).add(" \nFROM transcription_history");
    std::vector<Fragment> clauses;
    clauses.push_back(Fragment{}.add("transcription_history.id = ").bind(std::string(id)));
    for (Fragment& clause : filter_clauses("transcription_history",
                                           "transcription_history.timestamp", {include_remote})) {
        clauses.push_back(std::move(clause));
    }
    query.add(where(std::move(clauses)));
    Row row;
    {
        sql::Connection connection = open_read_only(path_);
        row = one(connection, query);
    }
    Transcription detail;
    detail.id = text(row[0]);
    detail.title = optional_text(row[1]);
    detail.timestamp = text(row[2]);
    detail.model = text(row[3]);
    detail.source_name = optional_text(row[4]);
    detail.audio_duration = optional_real(row[5]);
    detail.origin_device_id = optional_text(row[6]);
    detail.origin_device_name = optional_text(row[7]);
    detail.text = text(row[8]);
    detail.raw_text = optional_text(row[9]);
    detail.cleanup_provider = optional_text(row[10]);
    detail.cleanup_model = optional_text(row[11]);
    detail.preview = text(row[12]);
    return detail.to_json();
}

Json HistoryStore::meetings(const ListQuery& request) {
    const Filters filters{request.include_remote, request.since, request.before,
                          request.origin_device_id};
    Fragment inner;
    inner.add("SELECT ").add(kMeetingColumns).add(time_of("meeting_sessions.started_at"));
    inner.add(" AS sort_time \nFROM meeting_sessions");
    std::vector<Fragment> clauses =
        filter_clauses("meeting_sessions", "meeting_sessions.started_at", filters);
    if (request.q && !request.q->empty()) {
        clauses.push_back(match("meeting_sessions.title", *request.q));
    }
    inner.add(where(std::move(clauses)));

    PageSpec spec;
    spec.columns = {"id",       "title",     "status",           "started_at",
                    "ended_at", "state_seq", "origin_device_id", "origin_device_name",
                    "sort_time"};
    spec.keys = {"sort_time", "id"};
    spec.descending = {true, false};
    spec.scope = Json::array({"meetings", optional_json(request.q), filters_json(filters)});
    return page(path_, inner, spec, request.limit, request.cursor, meeting_from);
}

Json HistoryStore::meeting(std::string_view id, bool include_remote) {
    sql::Connection connection = open_read_only(path_);
    return meeting_from(one(connection, meeting_detail(id, include_remote)));
}

Json HistoryStore::segments(const SegmentQuery& request) {
    meeting(request.meeting_id, request.include_remote);
    Fragment inner;
    inner.add("SELECT meeting_segments.id AS id, meeting_segments.meeting_id AS meeting_id, ");
    inner.add("meeting_segments.start_s AS start_s, meeting_segments.end_s AS end_s, ");
    inner.add("meeting_segments.channel AS channel, meeting_segments.text AS text, ");
    inner.add("meeting_segments.speaker_participant_id AS speaker_participant_id, ");
    inner.add("meeting_segments.speaker_source AS speaker_source \nFROM meeting_segments");
    std::vector<Fragment> clauses;
    clauses.push_back(Fragment{}.add("meeting_segments.meeting_id = ").bind(request.meeting_id));
    if (request.start_s) {
        clauses.push_back(Fragment{}.add("meeting_segments.start_s >= ").bind(*request.start_s));
    }
    if (request.end_s) {
        clauses.push_back(Fragment{}.add("meeting_segments.start_s < ").bind(*request.end_s));
    }
    inner.add(where(std::move(clauses)));

    const auto bound = [](const std::optional<double>& value) {
        return value ? Json(*value) : Json(nullptr);
    };
    PageSpec spec;
    spec.columns = {"id",
                    "meeting_id",
                    "start_s",
                    "end_s",
                    "channel",
                    "text",
                    "speaker_participant_id",
                    "speaker_source"};
    spec.keys = {"start_s", "id"};
    spec.descending = {false, false};
    // Visibility is checked on the parent above, not on segment rows. The local-only scope is
    // what earlier clients hold, so the same cursor also works on the meeting's remote copy.
    spec.scope = Json::array(
        {"segments", request.meeting_id, bound(request.start_s), bound(request.end_s), false});
    spec.compatible_scopes = {Json::array(
        {"segments", request.meeting_id, bound(request.start_s), bound(request.end_s), true})};
    return page(path_, inner, spec, request.limit, request.cursor, segment_from);
}

Json HistoryStore::segment(std::string_view meeting_id, std::string_view segment_id,
                           bool include_remote) {
    Fragment query;
    query.add("SELECT ").add(kSegmentColumns).add(" \nFROM meeting_segments JOIN ");
    query.add("meeting_sessions ON meeting_sessions.id = meeting_segments.meeting_id");
    std::vector<Fragment> clauses;
    clauses.push_back(
        Fragment{}.add("meeting_segments.meeting_id = ").bind(std::string(meeting_id)));
    clauses.push_back(Fragment{}.add("meeting_segments.id = ").bind(std::string(segment_id)));
    for (Fragment& clause :
         filter_clauses("meeting_sessions", "meeting_sessions.started_at", {include_remote})) {
        clauses.push_back(std::move(clause));
    }
    query.add(where(std::move(clauses)));
    sql::Connection connection = open_read_only(path_);
    return segment_from(one(connection, query));
}

Json HistoryStore::insights(std::string_view meeting_id, bool include_remote) {
    Fragment query;
    query.add("SELECT meeting_sessions.state_json, meeting_sessions.state_seq \nFROM ");
    query.add("meeting_sessions");
    std::vector<Fragment> clauses;
    clauses.push_back(Fragment{}.add("meeting_sessions.id = ").bind(std::string(meeting_id)));
    for (Fragment& clause :
         filter_clauses("meeting_sessions", "meeting_sessions.started_at", {include_remote})) {
        clauses.push_back(std::move(clause));
    }
    query.add(where(std::move(clauses)));
    Row row;
    {
        sql::Connection connection = open_read_only(path_);
        row = one(connection, query);
    }
    MeetingInsights base;
    base.meeting_id = std::string(meeting_id);
    base.state_seq = integer(row[1]);
    const std::optional<std::string> state_json = optional_text(row[0]);
    base.snapshot_available = state_json && !state_json->empty();
    if (!base.snapshot_available) {
        return base.to_json();
    }
    try {
        return snapshot_insights(py::loads(*state_json), std::move(base)).to_json();
    } catch (const py::ValueError&) {
    } catch (const ModelError&) {
    }
    throw SnapshotUnavailable(
        "Stored meeting insights could not be read; the transcript is still available.");
}

Json HistoryStore::search(const SearchQuery& request) {
    const std::string& q = request.q;
    if (q.empty() || py::strip(q).empty() || py::len(q) > 500) {
        throw InvalidQuery("q must contain between 1 and 500 characters.");
    }
    if (request.kind != "all" && request.kind != "transcription" && request.kind != "meeting") {
        throw InvalidQuery("Unknown search kind.");
    }
    const Filters filters{request.include_remote, request.since, request.before,
                          request.origin_device_id};
    std::vector<Fragment> statements;
    if (request.kind == "all" || request.kind == "transcription") {
        // The matched field and its text, in the order text, raw_text, title, else source_name.
        const auto chosen = [&q](bool labels) {
            Fragment out;
            out.add("CASE");
            const std::array<std::string_view, 3> fields = {"text", "raw_text", "title"};
            for (const std::string_view field : fields) {
                const std::string column = "transcription_history." + std::string(field);
                out.add(" WHEN ").add(match(column, q)).add(" THEN ");
                if (labels) {
                    out.bind(std::string(field));
                } else {
                    out.add(column);
                }
            }
            out.add(" ELSE ");
            if (labels) {
                out.bind(std::string("source_name"));
            } else {
                out.add("transcription_history.source_name");
            }
            return out.add(" END");
        };
        Fragment statement;
        statement.add("SELECT ").bind(std::string("transcription")).add(" AS kind, ");
        statement.add("transcription_history.id AS id, ").bind(nullptr).add(" AS meeting_id, ");
        statement.add("coalesce(transcription_history.title, transcription_history.source_name, ");
        statement.bind(std::string("Transcription")).add(") AS title, ");
        statement.add("transcription_history.timestamp AS timestamp, ");
        statement.add(excerpt(chosen(false), q)).add(" AS excerpt, ");
        statement.add(chosen(true)).add(" AS matched_field, ").bind(nullptr);
        statement.add(" AS start_s, ").bind(nullptr).add(" AS end_s, ");
        statement.add("transcription_history.origin_device_id AS origin_device_id, ");
        statement.add(time_of("transcription_history.timestamp"));
        statement.add(" AS sort_time \nFROM transcription_history");
        std::vector<Fragment> matches;
        for (const std::string_view field : {"text", "raw_text", "source_name", "title"}) {
            matches.push_back(match("transcription_history." + std::string(field), q));
        }
        std::vector<Fragment> clauses;
        clauses.push_back(Fragment{}.add("(").add(join(matches, " OR ")).add(")"));
        for (Fragment& clause :
             filter_clauses("transcription_history", "transcription_history.timestamp", filters)) {
            clauses.push_back(std::move(clause));
        }
        statements.push_back(statement.add(where(std::move(clauses))));
    }
    if (request.kind == "all" || request.kind == "meeting") {
        Fragment meeting;
        meeting.add("SELECT ").bind(std::string("meeting")).add(" AS kind, ");
        meeting.add("meeting_sessions.id AS id, meeting_sessions.id AS meeting_id, ");
        meeting.add("meeting_sessions.title AS title, meeting_sessions.started_at AS timestamp, ");
        meeting.add(excerpt(Fragment{}.add("meeting_sessions.title"), q)).add(" AS excerpt, ");
        meeting.bind(std::string("title")).add(" AS matched_field, ").bind(nullptr);
        meeting.add(" AS start_s, ").bind(nullptr).add(" AS end_s, ");
        meeting.add("meeting_sessions.origin_device_id AS origin_device_id, ");
        meeting.add(time_of("meeting_sessions.started_at"));
        meeting.add(" AS sort_time \nFROM meeting_sessions");
        std::vector<Fragment> clauses;
        clauses.push_back(match("meeting_sessions.title", q));
        for (Fragment& clause :
             filter_clauses("meeting_sessions", "meeting_sessions.started_at", filters)) {
            clauses.push_back(std::move(clause));
        }
        statements.push_back(meeting.add(where(std::move(clauses))));

        Fragment segment;
        segment.add("SELECT ").bind(std::string("segment")).add(" AS kind, ");
        segment.add("meeting_segments.id AS id, meeting_segments.meeting_id AS meeting_id, ");
        segment.add("meeting_sessions.title AS title, meeting_sessions.started_at AS timestamp, ");
        segment.add(excerpt(Fragment{}.add("meeting_segments.text"), q)).add(" AS excerpt, ");
        segment.bind(std::string("text")).add(" AS matched_field, ");
        segment.add("meeting_segments.start_s AS start_s, meeting_segments.end_s AS end_s, ");
        segment.add("meeting_sessions.origin_device_id AS origin_device_id, ");
        segment.add(time_of("meeting_sessions.started_at"));
        segment.add(" AS sort_time \nFROM meeting_segments JOIN meeting_sessions ON ");
        segment.add("meeting_sessions.id = meeting_segments.meeting_id");
        std::vector<Fragment> segment_clauses;
        segment_clauses.push_back(match("meeting_segments.text", q));
        for (Fragment& clause :
             filter_clauses("meeting_sessions", "meeting_sessions.started_at", filters)) {
            segment_clauses.push_back(std::move(clause));
        }
        statements.push_back(segment.add(where(std::move(segment_clauses))));
    }

    PageSpec spec;
    spec.columns = {
        "kind",    "id",    "meeting_id",       "title",    "timestamp", "excerpt", "matched_field",
        "start_s", "end_s", "origin_device_id", "sort_time"};
    spec.keys = {"sort_time", "kind", "id"};
    spec.descending = {true, false, false};
    spec.scope = Json::array({"search", q, request.kind, filters_json(filters)});
    return page(path_, join(statements, " UNION ALL "), spec, request.limit, request.cursor,
                [](const Row& row) {
                    SearchHit hit;
                    hit.kind = text(row[0]);
                    hit.id = text(row[1]);
                    hit.meeting_id = optional_text(row[2]);
                    hit.title = text(row[3]);
                    hit.timestamp = text(row[4]);
                    hit.excerpt = text(row[5]);
                    hit.matched_field = text(row[6]);
                    hit.start_s = optional_real(row[7]);
                    hit.end_s = optional_real(row[8]);
                    hit.origin_device_id = optional_text(row[9]);
                    // with_resource: a path that fetches the hit, ids quoted with safe="".
                    if (hit.kind == "transcription") {
                        hit.resource = "/v1/transcriptions/" + quote(hit.id);
                    } else {
                        hit.resource = "/v1/meetings/" + quote(hit.meeting_id.value_or(""));
                        if (hit.kind == "segment") {
                            hit.resource += "/segments/" + quote(hit.id);
                        }
                    }
                    if (hit.origin_device_id) {
                        hit.resource += "?include_remote=true";
                    }
                    return hit.to_json();
                });
}

} // namespace fv::agentapi
