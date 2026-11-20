#include "history/repository.h"

#include <algorithm>
#include <stdexcept>

#include "db/schema.h"
#include "history/detail/rows.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv {

namespace {

namespace th = schema::transcription_history;
using history_detail::optional_text;
using history_detail::read_entry;
using history_detail::select_columns;

sql::Value text_or_null(const std::optional<std::string>& value) {
    if (value) {
        return *value;
    }
    return nullptr;
}

sql::Value real_or_null(const std::optional<double>& value) {
    if (value) {
        return *value;
    }
    return nullptr;
}

sql::Value integer_or_null(const std::optional<std::int64_t>& value) {
    if (value) {
        return *value;
    }
    return nullptr;
}

// Every column of `entry` by name, in table order.
std::vector<std::pair<std::string_view, sql::Value>> column_values(const HistoryEntry& entry) {
    return {
        {th::id, entry.id},
        {th::text, entry.text},
        {th::raw_text, text_or_null(entry.raw_text)},
        {th::timestamp, entry.timestamp},
        {th::model, entry.model},
        {th::audio_file, text_or_null(entry.audio_file)},
        {th::transcription_time, real_or_null(entry.transcription_time)},
        {th::audio_duration, real_or_null(entry.audio_duration)},
        {th::file_size, integer_or_null(entry.file_size)},
        {th::cleanup_provider, text_or_null(entry.cleanup_provider)},
        {th::cleanup_model, text_or_null(entry.cleanup_model)},
        {th::source_name, text_or_null(entry.source_name)},
        {th::title, text_or_null(entry.title)},
        {th::origin_device_id, text_or_null(entry.origin_device_id)},
        {th::origin_device_name, text_or_null(entry.origin_device_name)},
        {th::app_id, text_or_null(entry.app_id)},
        {th::app_name, text_or_null(entry.app_name)},
        {th::app_category, text_or_null(entry.app_category)},
        {th::cleanup_level, text_or_null(entry.cleanup_level)},
        {th::entry_kind, text_or_null(entry.entry_kind)},
        {th::cleaned_text, text_or_null(entry.cleaned_text)},
        {th::language, text_or_null(entry.language)},
    };
}

void insert(sql::Connection& conn, const std::vector<std::pair<std::string_view, sql::Value>>& row,
            std::string_view conflict_clause) {
    std::string names;
    std::string params;
    for (const auto& [name, value] : row) {
        names += names.empty() ? "" : ", ";
        names += name;
        params += params.empty() ? "?" : ", ?";
    }
    std::string text = "INSERT INTO transcription_history (" + names + ") VALUES (" + params + ")";
    text += conflict_clause;
    sql::Statement statement = conn.prepare(text);
    int index = 1;
    for (const auto& [name, value] : row) {
        statement.bind(index++, value);
    }
    statement.step();
}

std::vector<HistoryEntry> collect(sql::Statement& statement) {
    std::vector<HistoryEntry> entries;
    while (statement.step()) {
        entries.push_back(read_entry(statement));
    }
    return entries;
}

std::string origin_clause(const HistoryOrigin& origin) {
    switch (origin.kind) {
    case HistoryOrigin::Kind::Own:
        return "transcription_history.origin_device_id IS NULL";
    case HistoryOrigin::Kind::Device:
        return "transcription_history.origin_device_id = :origin";
    case HistoryOrigin::Kind::Any:
        break;
    }
    return "";
}

void bind_origin(sql::Statement& statement, const HistoryOrigin& origin) {
    if (origin.kind == HistoryOrigin::Kind::Device) {
        statement.bind(":origin", origin.device_id);
    }
}

// `if limit:` in Python: 0 and None both mean no limit.
std::string limit_clause(std::optional<std::int64_t> limit) {
    return limit && *limit != 0 ? " LIMIT :limit OFFSET 0" : "";
}

void bind_limit(sql::Statement& statement, std::optional<std::int64_t> limit) {
    if (limit && *limit != 0) {
        statement.bind(":limit", *limit);
    }
}

std::optional<HistoryEntry> entry_by_id(sql::Connection& conn, std::string_view entry_id) {
    sql::Statement statement =
        conn.prepare(select_columns() + " WHERE transcription_history.id = :id");
    statement.bind(":id", std::string(entry_id));
    if (!statement.step()) {
        return std::nullopt;
    }
    return read_entry(statement);
}

bool is_column(std::string_view name) {
    return std::ranges::find(th::columns, name) != th::columns.end();
}

} // namespace

void HistoryRepository::add_history_entry(const HistoryEntry& entry) {
    auto row = column_values(entry);
    // Python's add_history_entry takes no title or origin: they stay NULL.
    for (auto& [name, value] : row) {
        if (name == th::title || name == th::origin_device_id || name == th::origin_device_name) {
            value = nullptr;
        }
    }
    database_->write([&](sql::Connection& conn) {
        sql::Transaction tx = conn.begin();
        insert(conn, row, "");
        tx.commit();
    });
}

void HistoryRepository::add_history_entry(const HistoryEntry& entry, const ContextFields& context) {
    const ContextFields kept = history_context(context);
    HistoryEntry merged = entry;
    for (const std::string_view column : HISTORY_CONTEXT_COLUMNS) {
        *merged.context_column(column) = std::nullopt;
    }
    for (const auto& [key, value] : kept) {
        *merged.context_column(key) = value;
    }
    add_history_entry(merged);
}

std::vector<HistoryEntry> HistoryRepository::get_history_entries(std::optional<std::int64_t> limit,
                                                                 const HistoryOrigin& origin) {
    std::string text = select_columns();
    if (const std::string where = origin_clause(origin); !where.empty()) {
        text += " WHERE " + where;
    }
    text += " ORDER BY transcription_history.timestamp DESC" + limit_clause(limit);
    return database_->read([&](sql::Connection& conn) {
        sql::Statement statement = conn.prepare(text);
        bind_origin(statement, origin);
        bind_limit(statement, limit);
        return collect(statement);
    });
}

std::vector<HistoryEntry>
HistoryRepository::search_history_entries(std::string_view query, std::optional<std::int64_t> limit,
                                          const HistoryOrigin& origin) {
    const std::string needle = py::strip(query);
    if (needle.empty()) {
        return get_history_entries(limit, origin);
    }
    std::string escaped;
    for (const char c : needle) {
        if (c == '\\' || c == '%' || c == '_') {
            escaped += '\\';
        }
        escaped += c;
    }
    const std::string pattern = "%" + escaped + "%";

    std::string text = select_columns() + " WHERE ";
    if (const std::string where = origin_clause(origin); !where.empty()) {
        text += where + " AND ";
    }
    // SQLAlchemy's ilike on SQLite: lower(column) LIKE lower(pattern).
    std::string any;
    for (const std::string_view column : {th::text, th::raw_text, th::timestamp, th::source_name,
                                          th::title, th::origin_device_name, th::app_name}) {
        any += any.empty() ? "" : " OR ";
        any += "lower(transcription_history." + std::string(column) +
               ") LIKE lower(:pattern) ESCAPE '\\'";
    }
    text += "(" + any + ") ORDER BY transcription_history.timestamp DESC" + limit_clause(limit);
    return database_->read([&](sql::Connection& conn) {
        sql::Statement statement = conn.prepare(text);
        bind_origin(statement, origin);
        statement.bind(":pattern", pattern);
        bind_limit(statement, limit);
        return collect(statement);
    });
}

std::vector<HistoryEntry> HistoryRepository::get_history_entries_between(std::string_view start,
                                                                         std::string_view end) {
    return database_->read([&](sql::Connection& conn) {
        sql::Statement statement = conn.prepare(
            select_columns() +
            " WHERE transcription_history.timestamp >= :start AND transcription_history.timestamp "
            "< :end ORDER BY transcription_history.timestamp");
        statement.bind(":start", std::string(start));
        statement.bind(":end", std::string(end));
        return collect(statement);
    });
}

std::vector<HistoryCalendarRow> HistoryRepository::history_calendar_rows() {
    return database_->read([&](sql::Connection& conn) {
        sql::Statement statement = conn.prepare(
            "SELECT transcription_history.id, transcription_history.timestamp, "
            "transcription_history.source_name, transcription_history.origin_device_name, "
            "transcription_history.audio_duration, transcription_history.entry_kind FROM "
            "transcription_history");
        std::vector<HistoryCalendarRow> rows;
        while (statement.step()) {
            HistoryCalendarRow row;
            row.id = optional_text(statement.column(0)).value_or("");
            row.timestamp = optional_text(statement.column(1)).value_or("");
            row.source_name = optional_text(statement.column(2));
            row.origin_device_name = optional_text(statement.column(3));
            row.audio_duration = history_detail::optional_real(statement.column(4));
            row.entry_kind = optional_text(statement.column(5));
            rows.push_back(std::move(row));
        }
        return rows;
    });
}

std::optional<HistoryEntry> HistoryRepository::get_history_entry_by_id(std::string_view entry_id) {
    return database_->read([&](sql::Connection& conn) { return entry_by_id(conn, entry_id); });
}

std::optional<HistoryEntry>
HistoryRepository::get_history_entry_by_id(std::string_view entry_id, const HistoryOrigin& origin) {
    std::string text = select_columns() + " WHERE transcription_history.id = :id";
    if (const std::string where = origin_clause(origin); !where.empty()) {
        text += " AND " + where;
    }
    return database_->read([&](sql::Connection& conn) -> std::optional<HistoryEntry> {
        sql::Statement statement = conn.prepare(text);
        statement.bind(":id", std::string(entry_id));
        bind_origin(statement, origin);
        if (!statement.step()) {
            return std::nullopt;
        }
        return read_entry(statement);
    });
}

std::optional<HistoryEntry> HistoryRepository::get_failed_recording(std::string_view audio_file) {
    return database_->read([&](sql::Connection& conn) -> std::optional<HistoryEntry> {
        sql::Statement statement = conn.prepare(
            select_columns() + " WHERE transcription_history.audio_file = :audio_file AND "
                               "transcription_history.origin_device_id IS NULL AND "
                               "transcription_history.entry_kind = :entry_kind LIMIT 1 OFFSET 0");
        statement.bind(":audio_file", std::string(audio_file));
        statement.bind(":entry_kind", std::string(FAILED_RECORDING_KIND));
        if (!statement.step()) {
            return std::nullopt;
        }
        return read_entry(statement);
    });
}

std::optional<HistoryEntry>
HistoryRepository::complete_failed_recording(std::string_view entry_id,
                                             const HistoryColumnValues& fields) {
    for (const auto& [name, value] : fields) {
        if (!is_column(name)) {
            throw std::invalid_argument("Not a transcription_history column: " + name);
        }
    }
    const std::string filter = " WHERE transcription_history.id = :id AND "
                               "transcription_history.origin_device_id IS NULL AND "
                               "transcription_history.entry_kind = :entry_kind";
    return database_->write([&](sql::Connection& conn) -> std::optional<HistoryEntry> {
        sql::Transaction tx = conn.begin();
        std::int64_t rows = 0;
        if (fields.empty()) {
            sql::Statement statement =
                conn.prepare("SELECT count(*) FROM transcription_history" + filter);
            statement.bind(":id", std::string(entry_id));
            statement.bind(":entry_kind", std::string(FAILED_RECORDING_KIND));
            statement.step();
            rows = std::get<std::int64_t>(statement.column(0));
        } else {
            std::string assignments;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                assignments += assignments.empty() ? "" : ", ";
                assignments += fields[i].first + " = :v" + std::to_string(i);
            }
            sql::Statement statement =
                conn.prepare("UPDATE transcription_history SET " + assignments + filter);
            for (std::size_t i = 0; i < fields.size(); ++i) {
                statement.bind(":v" + std::to_string(i), fields[i].second);
            }
            statement.bind(":id", std::string(entry_id));
            statement.bind(":entry_kind", std::string(FAILED_RECORDING_KIND));
            statement.step();
            rows = conn.changes();
        }
        std::optional<HistoryEntry> entry;
        if (rows != 0) {
            entry = entry_by_id(conn, entry_id);
        }
        tx.commit();
        return entry;
    });
}

bool HistoryRepository::delete_history_entry(std::string_view entry_id) {
    return database_->write([&](sql::Connection& conn) {
        sql::Transaction tx = conn.begin();
        sql::Statement statement =
            conn.prepare("DELETE FROM transcription_history WHERE transcription_history.id = :id");
        statement.bind(":id", std::string(entry_id));
        statement.step();
        const bool deleted = conn.changes() != 0;
        tx.commit();
        return deleted;
    });
}

std::optional<HistoryEntry> HistoryRepository::update_history_entry(std::string_view entry_id,
                                                                    const HistoryEdits& fields) {
    std::string unknown;
    for (const auto& [name, value] : fields) {
        if (name != th::text && name != th::cleaned_text) {
            unknown += unknown.empty() ? name : ", " + name;
        }
    }
    if (!unknown.empty()) {
        throw py::TypeError("Not editable history fields: " + unknown);
    }
    if (const auto text = fields.find(th::text); text != fields.end() && !text->second) {
        throw py::TypeError("text can't be cleared");
    }
    return database_->write([&](sql::Connection& conn) -> std::optional<HistoryEntry> {
        sql::Transaction tx = conn.begin();
        if (!entry_by_id(conn, entry_id)) {
            return std::nullopt;
        }
        if (!fields.empty()) {
            std::string assignments;
            for (const auto& [name, value] : fields) {
                assignments += assignments.empty() ? "" : ", ";
                assignments += name + " = :" + name;
            }
            sql::Statement statement =
                conn.prepare("UPDATE transcription_history SET " + assignments + " WHERE id = :id");
            for (const auto& [name, value] : fields) {
                statement.bind(":" + name, text_or_null(value));
            }
            statement.bind(":id", std::string(entry_id));
            statement.step();
        }
        std::optional<HistoryEntry> updated = entry_by_id(conn, entry_id);
        tx.commit();
        return updated;
    });
}

void HistoryRepository::put_history_entry(const HistoryEntry& entry) {
    const auto row = column_values(entry);
    std::string updates;
    for (const auto& [name, value] : row) {
        if (name == th::id) {
            continue;
        }
        updates += updates.empty() ? "" : ", ";
        updates += std::string(name) + " = excluded." + std::string(name);
    }
    database_->write([&](sql::Connection& conn) {
        sql::Transaction tx = conn.begin();
        insert(conn, row, " ON CONFLICT(id) DO UPDATE SET " + updates);
        tx.commit();
    });
}

std::vector<OriginCount> HistoryRepository::history_origin_counts() {
    return database_->read([&](sql::Connection& conn) {
        sql::Statement statement = conn.prepare(
            "SELECT transcription_history.origin_device_id, "
            "max(transcription_history.origin_device_name) AS max_1, "
            "count(transcription_history.id) AS count_1 FROM transcription_history WHERE "
            "transcription_history.origin_device_id IS NOT NULL GROUP BY "
            "transcription_history.origin_device_id");
        std::vector<OriginCount> counts;
        while (statement.step()) {
            OriginCount count;
            count.device_id = optional_text(statement.column(0)).value_or("");
            count.name = optional_text(statement.column(1)).value_or("");
            count.count = std::get<std::int64_t>(statement.column(2));
            counts.push_back(std::move(count));
        }
        return counts;
    });
}

int HistoryRepository::rename_history_origin(std::string_view device_id, std::string_view name) {
    return database_->write([&](sql::Connection& conn) {
        sql::Transaction tx = conn.begin();
        sql::Statement statement =
            conn.prepare("UPDATE transcription_history SET origin_device_name=:name WHERE "
                         "transcription_history.origin_device_id = :device");
        statement.bind(":name", std::string(name));
        statement.bind(":device", std::string(device_id));
        statement.step();
        const auto changed = static_cast<int>(conn.changes());
        tx.commit();
        return changed;
    });
}

void HistoryRepository::clear_history() {
    database_->write([&](sql::Connection& conn) {
        sql::Transaction tx = conn.begin();
        conn.exec("DELETE FROM transcription_history WHERE "
                  "transcription_history.origin_device_id IS NULL");
        tx.commit();
    });
}

void HistoryRepository::clear_history_audio_file(std::string_view audio_file) {
    database_->write([&](sql::Connection& conn) {
        sql::Transaction tx = conn.begin();
        sql::Statement statement =
            conn.prepare("UPDATE transcription_history SET audio_file = NULL WHERE "
                         "transcription_history.audio_file = :audio_file");
        statement.bind(":audio_file", std::string(audio_file));
        statement.step();
        tx.commit();
    });
}

} // namespace fv
