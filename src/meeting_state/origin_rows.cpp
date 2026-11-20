#include "meeting_state/origin_rows.h"

#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "db/database.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/sql_internal.h"
#include "pyport/errors.h"
#include "sql/db.h"

namespace fv::meeting_state {

namespace {

// How SQLAlchemy reads and binds a column.
enum class Type { Text, Integer, Real, Boolean, Blob };

struct Column {
    std::string_view name;
    Type type;
    // The ORM's Python-side default as JSON text; empty when the column has none.
    std::string_view default_json = {};
};

struct Table {
    std::string_view key;   // in export_meeting_rows's result and replace_meeting_rows's rows
    std::string_view name;  // the SQL table
    std::string_view model; // the ORM class, for py::TypeError messages
    std::span<const Column> columns;
};

constexpr std::array kSession = {
    Column{"id", Type::Text},
    Column{"title", Type::Text, R"("")"},
    Column{"status", Type::Text},
    Column{"started_at", Type::Text},
    Column{"ended_at", Type::Text},
    Column{"paused_total_s", Type::Real, "0.0"},
    Column{"host_token", Type::Text},
    Column{"guest_token", Type::Text},
    Column{"cloud_enabled", Type::Boolean, "false"},
    Column{"asr_model", Type::Text},
    Column{"asr_remote_json", Type::Text},
    Column{"agent_provider", Type::Text},
    Column{"agent_model", Type::Text},
    Column{"agent_endpoint_json", Type::Text},
    Column{"spool_dir", Type::Text},
    Column{"state_json", Type::Text},
    Column{"state_seq", Type::Integer, "0"},
    Column{"app_pid", Type::Integer},
    Column{"app_heartbeat_at", Type::Text},
    Column{"origin_device_id", Type::Text},
    Column{"origin_device_name", Type::Text},
};

constexpr std::array kChunks = {
    Column{"id", Type::Integer},
    Column{"meeting_id", Type::Text},
    Column{"channel", Type::Text},
    Column{"seq", Type::Integer},
    Column{"file_path", Type::Text},
    Column{"start_s", Type::Real},
    Column{"duration_s", Type::Real},
    Column{"sample_rate", Type::Integer},
    Column{"asr_status", Type::Text, R"("pending")"},
    Column{"asr_attempts", Type::Integer, "0"},
    Column{"asr_error", Type::Text},
};

constexpr std::array kSegments = {
    Column{"id", Type::Text},
    Column{"meeting_id", Type::Text},
    Column{"chunk_id", Type::Integer},
    Column{"channel", Type::Text},
    Column{"start_s", Type::Real},
    Column{"end_s", Type::Real},
    Column{"text", Type::Text},
    Column{"speaker_participant_id", Type::Text},
    Column{"speaker_source", Type::Text, R"("channel")"},
    Column{"speaker_pinned", Type::Boolean, "false"},
    Column{"embedding", Type::Blob},
    Column{"created_at", Type::Text},
};

constexpr std::array kParticipants = {
    Column{"id", Type::Text},
    Column{"meeting_id", Type::Text},
    Column{"display_name", Type::Text},
    Column{"kind", Type::Text},
    Column{"name_source", Type::Text, R"("default")"},
    Column{"is_provisional", Type::Boolean, "false"},
    Column{"created_at", Type::Text},
    Column{"updated_at", Type::Text},
};

constexpr std::array kStateItems = {
    Column{"id", Type::Text},
    Column{"meeting_id", Type::Text},
    Column{"card", Type::Text},
    Column{"text", Type::Text},
    Column{"data_json", Type::Text},
    Column{"status", Type::Text},
    Column{"author_type", Type::Text},
    Column{"author_id", Type::Text},
    Column{"pinned", Type::Boolean, "false"},
    Column{"revision", Type::Integer, "1"},
    Column{"evidence_json", Type::Text, R"("[]")"},
    Column{"created_at", Type::Text},
    Column{"updated_at", Type::Text},
};

constexpr std::array kQuestions = {
    Column{"id", Type::Text},
    Column{"meeting_id", Type::Text},
    Column{"text", Type::Text},
    Column{"status", Type::Text},
    Column{"answer_text", Type::Text},
    Column{"answer_source", Type::Text},
    Column{"answer_confidence", Type::Real},
    Column{"suggested_answer_text", Type::Text},
    Column{"suggested_confidence", Type::Real},
    Column{"thread_json", Type::Text, R"("[]")"},
    Column{"evidence_json", Type::Text, R"("[]")"},
    Column{"asked_at", Type::Text},
    Column{"resolved_at", Type::Text},
    Column{"resolved_by", Type::Text},
};

constexpr std::array kEvents = {
    Column{"id", Type::Integer},        Column{"meeting_id", Type::Text},
    Column{"seq", Type::Integer},       Column{"ts", Type::Text},
    Column{"actor_type", Type::Text},   Column{"actor_id", Type::Text},
    Column{"action", Type::Text},       Column{"target_id", Type::Text},
    Column{"payload_json", Type::Text}, Column{"inverse_json", Type::Text},
};

const Table kSessionTable{"session", "meeting_sessions", "MeetingSession", kSession};
const Table kChunkTable{"chunks", "meeting_audio_chunks", "MeetingAudioChunk", kChunks};
const Table kSegmentTable{"segments", "meeting_segments", "MeetingSegment", kSegments};

// repository.py _COPY_TABLES: the child tables in the order a whole-meeting copy writes them.
const std::array<Table, 6> kCopyTables = {
    kChunkTable,
    kSegmentTable,
    Table{"participants", "meeting_participants", "MeetingParticipant", kParticipants},
    Table{"state_items", "meeting_state_items", "MeetingStateItem", kStateItems},
    Table{"questions", "meeting_questions", "MeetingQuestion", kQuestions},
    Table{"events", "meeting_events", "MeetingEvent", kEvents},
};

// delete_meeting's order: children before the chunks segments refer to.
constexpr std::array<std::string_view, 6> kDeleteOrder = {
    "meeting_events",       "meeting_questions", "meeting_state_items",
    "meeting_participants", "meeting_segments",  "meeting_audio_chunks"};

std::string select_list(const Table& table) {
    std::string text;
    for (const Column& column : table.columns) {
        text += text.empty() ? "" : ", ";
        text += std::string(table.name) + "." + std::string(column.name);
    }
    return text;
}

// _columns(row): {column.name: value} as SQLAlchemy reads each type.
Json read_row(const sql::Statement& statement, const Table& table) {
    Json row = Json::object();
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
        const Column& column = table.columns[index];
        const sql::Value value = statement.column(static_cast<int>(index));
        row[std::string(column.name)] = column.type == Type::Boolean
                                            ? detail::bool_from_value(value)
                                            : detail::from_value(value);
    }
    return row;
}

// Python's type(value).__name__ for a decoded JSON value.
std::string py_type_name(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
    case Json::value_t::discarded:
        return "NoneType";
    case Json::value_t::boolean:
        return "bool";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return "int";
    case Json::value_t::number_float:
        return "float";
    case Json::value_t::string:
        return "str";
    case Json::value_t::array:
        return "list";
    case Json::value_t::object:
        return "dict";
    case Json::value_t::binary:
        return "bytes";
    }
    return "object";
}

// An error a bind processor raised while the ORM flushed: SQLAlchemy wraps it in StatementError,
// whose first line names the original exception (the SQL and parameters lines that follow it in
// Python are left out).
[[noreturn]] void statement_error(std::string_view original, const std::string& message) {
    throw py::PyError("StatementError", "(builtins." + std::string(original) + ") " + message);
}

// SQLAlchemy's Boolean bind processor (_strict_as_bool): None, True and False, and the numbers
// equal to them; anything else is refused before it reaches SQLite.
sql::Value boolean_value(const Json& value) {
    if (value.is_null()) {
        return nullptr;
    }
    if (value.is_boolean()) {
        return std::int64_t{value.get<bool>() ? 1 : 0};
    }
    if (value.is_number()) {
        const double number = value.get<double>();
        if (number == 0.0 || number == 1.0) {
            return static_cast<std::int64_t>(number);
        }
        if (value.is_number_integer() || value.is_number_unsigned()) {
            statement_error("ValueError",
                            "Value " + detail::py_repr(value) + " is not None, True, or False");
        }
    }
    statement_error("TypeError", "Not a boolean value: " + detail::py_repr(value));
}

// LargeBinary binds through memoryview(value): bytes or None only.
sql::Value blob_value(const Json& value) {
    if (value.is_null()) {
        return nullptr;
    }
    if (!value.is_binary()) {
        statement_error("TypeError", "memoryview: a bytes-like object is required, not '" +
                                         py_type_name(value) + "'");
    }
    sql::Blob blob;
    blob.reserve(value.get_binary().size());
    for (const std::uint8_t byte : value.get_binary()) {
        blob.push_back(static_cast<std::byte>(byte));
    }
    return blob;
}

sql::Value bind_value(const Column& column, const Json& value) {
    switch (column.type) {
    case Type::Boolean:
        return boolean_value(value);
    case Type::Blob:
        return blob_value(value);
    case Type::Text:
    case Type::Integer:
    case Type::Real:
        break;
    }
    return detail::to_value(value);
}

const Column* find_column(const Table& table, std::string_view name) {
    for (const Column& column : table.columns) {
        if (column.name == name) {
            return &column;
        }
    }
    return nullptr;
}

// Model(**values), before anything reaches the database: the call's own check that `values` is
// a mapping, then SQLAlchemy's declarative constructor refusing the first key that is no column.
void construct(const Table& table, const Json& values) {
    if (!values.is_object()) {
        throw py::TypeError("services.models." + std::string(table.model) +
                            "() argument after ** must be a mapping, not " + py_type_name(values));
    }
    for (const auto& [key, value] : values.items()) {
        if (find_column(table, key) == nullptr) {
            throw py::TypeError("'" + key + "' is an invalid keyword argument for " +
                                std::string(table.model));
        }
    }
}

// The INSERT the flush of a constructed row runs: given columns as given, missing ones with a
// Python default take it, the rest are left to SQLite (NULL, or this database's own id for an
// integer key). Returns the row's rowid.
std::int64_t insert_row(sql::Connection& connection, const Table& table, const Json& values) {
    std::string names;
    std::vector<sql::Value> params;
    for (const Column& column : table.columns) {
        const auto given = values.find(std::string(column.name));
        if (given != values.end()) {
            // A None primary key is unassigned to the ORM: SQLite gives the row an id.
            if (column.name == "id" && column.type == Type::Integer && given->is_null()) {
                continue;
            }
            names += names.empty() ? "" : ", ";
            names += column.name;
            params.push_back(bind_value(column, *given));
        } else if (!column.default_json.empty()) {
            names += names.empty() ? "" : ", ";
            names += column.name;
            params.push_back(bind_value(column, Json::parse(column.default_json)));
        }
    }
    // With no value at all SQLite fills every column itself (and refuses the NOT NULL ones).
    sql::Statement statement = connection.prepare(
        names.empty() ? "INSERT INTO " + std::string(table.name) + " DEFAULT VALUES"
                      : "INSERT INTO " + std::string(table.name) + " (" + names + ") VALUES (" +
                            detail::placeholders(params.size()) + ")");
    detail::bind_all(statement, params);
    statement.step();
    return connection.last_insert_rowid();
}

// rows.get(key, ()): the rows under `key`, none when it is missing. A value that is not a list
// is refused (Python would iterate a string's characters or a dict's keys; the record kinds
// only pass lists).
const Json& rows_of(const Json& rows, std::string_view key) {
    static const Json none = Json::array();
    if (!rows.is_object()) {
        throw py::PyError("AttributeError",
                          "'" + py_type_name(rows) + "' object has no attribute 'get'");
    }
    const auto found = rows.find(std::string(key));
    if (found == rows.end()) {
        return none;
    }
    if (!found->is_array()) {
        throw py::TypeError("'" + py_type_name(*found) + "' object is not iterable");
    }
    return *found;
}

// dict(values): a copy the caller's rows never see changed. A row that is not an object is
// refused: a scalar with dict()'s own TypeError, a string or list with a TypeError of this
// function's (Python's dict() would take a list of pairs; the record kinds only pass objects).
Json dict_copy(const Json& values) {
    if (values.is_object()) {
        return values;
    }
    if (values.is_string() || values.is_array() || values.is_binary()) {
        throw py::TypeError("a meeting row must be an object, not " + py_type_name(values));
    }
    throw py::TypeError("'" + py_type_name(values) + "' object is not iterable");
}

// A chunk_ref as a dict key: Python's 1, 1.0 and True are one key.
// Keys are numbers, strings or null (check_hashable refuses the rest).
struct RefLess {
    static int rank(const Json& value) {
        if (value.is_number() || value.is_boolean()) {
            return 0;
        }
        return value.is_string() ? 1 : 2;
    }
    static double number(const Json& value) {
        if (value.is_boolean()) {
            return value.get<bool>() ? 1.0 : 0.0;
        }
        return value.get<double>();
    }
    bool operator()(const Json& a, const Json& b) const {
        const int rank_a = rank(a);
        const int rank_b = rank(b);
        if (rank_a != rank_b) {
            return rank_a < rank_b;
        }
        if (rank_a == 0) {
            return number(a) < number(b);
        }
        if (rank_a == 1) {
            return a.get_ref<const std::string&>() < b.get_ref<const std::string&>();
        }
        return false;
    }
};

void check_hashable(const Json& value) {
    if (value.is_array() || value.is_object() || value.is_binary()) {
        throw py::TypeError("unhashable type: '" + py_type_name(value) + "'");
    }
}

} // namespace

std::vector<RecordOwner> record_owners(Database& database) {
    return database.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(
            "SELECT meeting_sessions.origin_device_id, max(meeting_sessions.origin_device_name) "
            "AS max_1, count(meeting_sessions.id) AS count_1 FROM meeting_sessions WHERE "
            "meeting_sessions.origin_device_id IS NOT NULL GROUP BY "
            "meeting_sessions.origin_device_id");
        std::vector<RecordOwner> owners;
        while (statement.step()) {
            RecordOwner owner;
            owner.id = detail::text_value(statement.column(0));
            owner.name = detail::text_value(statement.column(1));
            if (owner.name.empty()) {
                owner.name = "Unnamed computer";
            }
            owner.count = detail::int_value(statement.column(2));
            owners.push_back(std::move(owner));
        }
        return owners;
    });
}

int rename_origin(Database& database, std::string_view device_id, std::string_view name) {
    return database.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        sql::Statement statement =
            connection.prepare("UPDATE meeting_sessions SET origin_device_name=? WHERE "
                               "meeting_sessions.origin_device_id = ?");
        detail::bind_all(statement, {std::string(name), std::string(device_id)});
        statement.step();
        const auto changed = static_cast<int>(connection.changes());
        transaction.commit();
        return changed;
    });
}

std::vector<OriginSpool> origin_spools(Database& database, std::string_view device_id) {
    return database.read([&](sql::Connection& connection) {
        sql::Statement statement =
            connection.prepare("SELECT meeting_sessions.id, meeting_sessions.spool_dir FROM "
                               "meeting_sessions WHERE meeting_sessions.origin_device_id = ?");
        statement.bind(1, std::string(device_id));
        std::vector<OriginSpool> spools;
        while (statement.step()) {
            spools.push_back(
                {detail::text_value(statement.column(0)), detail::text_value(statement.column(1))});
        }
        return spools;
    });
}

Json export_meeting_rows(Database& database, std::string_view meeting_id) {
    return database.read([&](sql::Connection& connection) -> Json {
        sql::Statement session = connection.prepare("SELECT " + select_list(kSessionTable) +
                                                    " FROM meeting_sessions WHERE "
                                                    "meeting_sessions.id = ?");
        session.bind(1, std::string(meeting_id));
        if (!session.step()) {
            return nullptr;
        }
        Json rows = Json::object();
        rows["session"] = read_row(session, kSessionTable);
        for (const Table& table : kCopyTables) {
            const std::string name(table.name);
            sql::Statement statement =
                connection.prepare("SELECT " + select_list(table) + " FROM " + name + " WHERE " +
                                   name + ".meeting_id = ? ORDER BY " + name + ".id");
            statement.bind(1, std::string(meeting_id));
            Json items = Json::array();
            while (statement.step()) {
                items.push_back(read_row(statement, table));
            }
            rows[std::string(table.key)] = std::move(items);
        }
        return rows;
    });
}

void replace_meeting_rows(Database& database, std::string_view meeting_id,
                          const Json& session_values, const Json& rows, bool publish_segments) {
    database.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin(sql::TxMode::Immediate);
        for (const std::string_view table : kDeleteOrder) {
            const std::string name(table);
            sql::Statement statement =
                connection.prepare("DELETE FROM " + name + " WHERE " + name + ".meeting_id = ?");
            statement.bind(1, std::string(meeting_id));
            statement.step();
        }
        sql::Statement session =
            connection.prepare("DELETE FROM meeting_sessions WHERE meeting_sessions.id = ?");
        session.bind(1, std::string(meeting_id));
        session.step();

        construct(kSessionTable, session_values);
        insert_row(connection, kSessionTable, session_values);

        // Chunk ids are this database's own: chunk_ref (the id on the other computer) maps
        // segments onto the new ones. Each chunk is flushed as it is added, for its new id.
        std::map<Json, std::int64_t, RefLess> chunk_ids;
        for (const Json& item : rows_of(rows, kChunkTable.key)) {
            Json values = dict_copy(item);
            const auto ref = values.find("chunk_ref");
            if (ref == values.end()) {
                detail::key_error("chunk_ref");
            }
            const Json key = *ref;
            values.erase(ref);
            construct(kChunkTable, values);
            const std::int64_t chunk_id = insert_row(connection, kChunkTable, values);
            check_hashable(key);
            chunk_ids[key] = chunk_id;
        }
        // Every other row is constructed first and written by the flush at commit, so a key
        // that is no column is refused before any of them reaches the database.
        std::vector<std::pair<const Table*, Json>> pending;
        for (const Json& item : rows_of(rows, kSegmentTable.key)) {
            Json values = dict_copy(item);
            const auto chunk = values.find("chunk_id");
            if (chunk != values.end() && !chunk->is_null()) {
                check_hashable(*chunk);
                const auto mapped = chunk_ids.find(*chunk);
                *chunk = mapped != chunk_ids.end() ? Json(mapped->second) : Json(nullptr);
            }
            construct(kSegmentTable, values);
            pending.emplace_back(&kSegmentTable, std::move(values));
        }
        for (std::size_t index = 2; index < kCopyTables.size(); ++index) {
            for (const Json& values : rows_of(rows, kCopyTables[index].key)) {
                construct(kCopyTables[index], values);
                pending.emplace_back(&kCopyTables[index], values);
            }
        }
        for (const auto& [table, values] : pending) {
            insert_row(connection, *table, values);
        }
        if (!publish_segments) {
            auto discard =
                connection.prepare("DELETE FROM meeting_segment_delivery WHERE meeting_id=?");
            discard.bind(1, std::string(meeting_id));
            discard.step();
        }
        transaction.commit();
    });
}

std::int64_t count_unfinished_chunks(Database& database, std::string_view meeting_id) {
    return database.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(
            "SELECT count(*) AS count_1 FROM meeting_audio_chunks WHERE "
            "meeting_audio_chunks.meeting_id = ? AND meeting_audio_chunks.asr_status != 'done'");
        statement.bind(1, std::string(meeting_id));
        statement.step();
        return detail::int_value(statement.column(0));
    });
}

} // namespace fv::meeting_state
