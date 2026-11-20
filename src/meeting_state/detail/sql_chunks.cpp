// SqlMeetingRepository: audio chunks, the durable capture and per-chunk ASR retry unit
// (repository.py register_chunk .. mark_chunks_done).

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "db/database.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/sql_internal.h"
#include "meeting_state/sql_repository.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

// MeetingAudioChunk's columns in table order.
constexpr std::array<std::string_view, 11> kColumns = {
    "id",         "meeting_id",  "channel",    "seq",          "file_path", "start_s",
    "duration_s", "sample_rate", "asr_status", "asr_attempts", "asr_error"};

// asr_error keeps at most this many code points.
constexpr std::int64_t kMaxErrorChars = 2000;

std::int64_t insert_chunk(sql::Connection& connection, const Json& fields) {
    if (!fields.is_object()) {
        throw py::TypeError("register_chunk() takes keyword arguments");
    }
    for (const auto& [key, value] : fields.items()) {
        if (std::find(kColumns.begin(), kColumns.end(), key) == kColumns.end()) {
            throw py::TypeError("'" + key +
                                "' is an invalid keyword argument for MeetingAudioChunk");
        }
    }
    std::string names;
    std::vector<sql::Value> values;
    for (const std::string_view column : kColumns) {
        const auto given = fields.find(std::string(column));
        Json value;
        if (given != fields.end()) {
            value = *given;
        } else if (column == "asr_status") {
            value = "pending";
        } else if (column == "asr_attempts") {
            value = 0;
        } else {
            continue;
        }
        names += names.empty() ? "" : ", ";
        names += column;
        values.push_back(detail::to_value(value));
    }
    sql::Statement statement =
        connection.prepare("INSERT INTO meeting_audio_chunks (" + names + ") VALUES (" +
                           detail::placeholders(values.size()) + ")");
    detail::bind_all(statement, values);
    statement.step();
    return connection.last_insert_rowid();
}

struct ChunkState {
    std::string asr_status;
    std::int64_t asr_attempts = 0;
};

std::optional<ChunkState> chunk_state(sql::Connection& connection, std::int64_t chunk_id) {
    sql::Statement statement =
        connection.prepare("SELECT meeting_audio_chunks.asr_status, "
                           "meeting_audio_chunks.asr_attempts FROM meeting_audio_chunks WHERE "
                           "meeting_audio_chunks.id = ?");
    statement.bind(1, chunk_id);
    if (!statement.step()) {
        return std::nullopt;
    }
    return ChunkState{detail::text_value(statement.column(0)),
                      detail::int_value(statement.column(1))};
}

std::vector<Json> chunk_rows(sql::Connection& connection, const std::string& sql,
                             const std::vector<sql::Value>& params) {
    sql::Statement statement = connection.prepare(sql);
    detail::bind_all(statement, params);
    std::vector<Json> rows;
    while (statement.step()) {
        rows.push_back(detail::chunk_dict(statement));
    }
    return rows;
}

// Ids and states of a meeting's chunks that are not done, in the order SQLite returns them.
std::vector<std::pair<std::int64_t, ChunkState>> unfinished(sql::Connection& connection,
                                                            std::string_view meeting_id) {
    sql::Statement statement = connection.prepare(
        "SELECT meeting_audio_chunks.id, meeting_audio_chunks.asr_status, "
        "meeting_audio_chunks.asr_attempts FROM meeting_audio_chunks WHERE "
        "meeting_audio_chunks.meeting_id = ? AND meeting_audio_chunks.asr_status != 'done'");
    statement.bind(1, std::string(meeting_id));
    std::vector<std::pair<std::int64_t, ChunkState>> rows;
    while (statement.step()) {
        rows.emplace_back(detail::int_value(statement.column(0)),
                          ChunkState{detail::text_value(statement.column(1)),
                                     detail::int_value(statement.column(2))});
    }
    return rows;
}

void exec_bound(sql::Connection& connection, std::string_view sql,
                const std::vector<sql::Value>& params) {
    sql::Statement statement = connection.prepare(sql);
    detail::bind_all(statement, params);
    statement.step();
}

} // namespace

std::int64_t SqlMeetingRepository::register_chunk(const Json& fields) {
    return database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        const std::int64_t id = insert_chunk(connection, fields);
        transaction.commit();
        return id;
    });
}

ChunkRegistration SqlMeetingRepository::register_chunk_if_missing(const Json& fields) {
    // Recovery can restart after the SQLite commit but before its sidecar file is removed; the
    // per-meeting, per-channel sequence is the durable identity.
    const std::string meeting_id = detail::py_str(detail::at(fields, "meeting_id"));
    const std::string channel = detail::py_str(detail::at(fields, "channel"));
    const std::int64_t seq = detail::py_int_or_throw(detail::at(fields, "seq"));
    return database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        sql::Statement statement = connection.prepare(
            "SELECT meeting_audio_chunks.id FROM meeting_audio_chunks WHERE "
            "meeting_audio_chunks.meeting_id = ? AND meeting_audio_chunks.channel = ? AND "
            "meeting_audio_chunks.seq = ?");
        detail::bind_all(statement, {meeting_id, channel, seq});
        if (statement.step()) {
            const std::int64_t id = detail::int_value(statement.column(0));
            if (statement.step()) {
                throw std::runtime_error("Multiple rows were found when one or none was required");
            }
            return ChunkRegistration{id, false};
        }
        const std::int64_t id = insert_chunk(connection, fields);
        transaction.commit();
        return ChunkRegistration{id, true};
    });
}

void SqlMeetingRepository::set_chunk_status(std::int64_t chunk_id, std::string_view status,
                                            std::optional<std::string_view> error) {
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        const std::optional<ChunkState> row = chunk_state(connection, chunk_id);
        if (!row) {
            return;
        }
        // done is terminal: a late callback cannot regress it.
        if (row->asr_status == "done" && status != "done") {
            return;
        }
        std::string sql = "UPDATE meeting_audio_chunks SET asr_status = ?";
        std::vector<sql::Value> values{std::string(status)};
        if (status == "processing") {
            sql += ", asr_attempts = ?";
            values.emplace_back(row->asr_attempts + 1);
        }
        if (error) {
            sql += ", asr_error = ?";
            values.emplace_back(py::slice(*error, 0, kMaxErrorChars));
        }
        values.emplace_back(chunk_id);
        exec_bound(connection, sql + " WHERE meeting_audio_chunks.id = ?", values);
        transaction.commit();
    });
}

std::vector<Json>
SqlMeetingRepository::get_pending_chunks(std::string_view meeting_id,
                                         std::optional<std::int64_t> limit,
                                         const std::vector<std::int64_t>& exclude_ids) {
    // processing counts as retryable: such a chunk belongs to a transcription that never
    // reported back. Attempts are spent on entering processing, so the budget stays bounded
    // across restarts.
    std::string sql = "SELECT " + std::string(detail::kChunkColumns) +
                      " FROM meeting_audio_chunks WHERE meeting_audio_chunks.meeting_id = ? AND "
                      "meeting_audio_chunks.asr_status IN ('pending', 'failed', 'processing') AND "
                      "meeting_audio_chunks.asr_attempts < ?";
    std::vector<sql::Value> params{std::string(meeting_id), MAX_CHUNK_ATTEMPTS};
    if (!exclude_ids.empty()) {
        sql += " AND (meeting_audio_chunks.id NOT IN (" + detail::placeholders(exclude_ids.size()) +
               "))";
        for (const std::int64_t id : exclude_ids) {
            params.emplace_back(id);
        }
    }
    sql += " ORDER BY meeting_audio_chunks.start_s, meeting_audio_chunks.id";
    if (limit) {
        if (*limit <= 0) {
            return {};
        }
        sql += " LIMIT ?";
        params.emplace_back(*limit);
    }
    return database_.read(
        [&](sql::Connection& connection) { return chunk_rows(connection, sql, params); });
}

std::int64_t SqlMeetingRepository::count_unfinished_chunks(std::string_view meeting_id) {
    return database_.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(
            "SELECT count(*) FROM meeting_audio_chunks WHERE meeting_audio_chunks.meeting_id = ? "
            "AND meeting_audio_chunks.asr_status != 'done'");
        statement.bind(1, std::string(meeting_id));
        statement.step();
        return detail::int_value(statement.column(0));
    });
}

std::int64_t SqlMeetingRepository::reset_unfinished_chunks(std::string_view meeting_id,
                                                           bool force) {
    return database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        std::int64_t reset = 0;
        for (const auto& [id, row] : unfinished(connection, meeting_id)) {
            // Startup recovery must not hot-loop exhausted or invalid chunks on every launch;
            // force is an explicit repair or retry.
            if (!force && (row.asr_status == "blocked" || row.asr_attempts >= MAX_CHUNK_ATTEMPTS)) {
                continue;
            }
            if (force) {
                exec_bound(connection,
                           "UPDATE meeting_audio_chunks SET asr_status = 'pending', "
                           "asr_attempts = 0, asr_error = NULL WHERE meeting_audio_chunks.id = ?",
                           {id});
            } else {
                exec_bound(connection,
                           "UPDATE meeting_audio_chunks SET asr_status = 'pending' WHERE "
                           "meeting_audio_chunks.id = ?",
                           {id});
            }
            ++reset;
        }
        transaction.commit();
        return reset;
    });
}

void SqlMeetingRepository::defer_chunk_after_connection_failure(std::int64_t chunk_id,
                                                                std::string_view error) {
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        const std::optional<ChunkState> row = chunk_state(connection, chunk_id);
        if (!row || row->asr_status == "done") {
            return;
        }
        exec_bound(connection,
                   "UPDATE meeting_audio_chunks SET asr_status = 'pending', asr_attempts = ?, "
                   "asr_error = ? WHERE meeting_audio_chunks.id = ?",
                   {std::max<std::int64_t>(0, row->asr_attempts - 1),
                    py::slice(error, 0, kMaxErrorChars), chunk_id});
        transaction.commit();
    });
}

std::vector<Json> SqlMeetingRepository::get_audio_chunks(std::string_view meeting_id) {
    return database_.read([&](sql::Connection& connection) {
        return chunk_rows(connection,
                          "SELECT " + std::string(detail::kChunkColumns) +
                              " FROM meeting_audio_chunks WHERE meeting_audio_chunks.meeting_id "
                              "= ? ORDER BY meeting_audio_chunks.start_s, "
                              "meeting_audio_chunks.channel, meeting_audio_chunks.seq",
                          {std::string(meeting_id)});
    });
}

std::int64_t SqlMeetingRepository::next_chunk_seq(std::string_view meeting_id,
                                                  std::string_view channel) {
    return database_.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(
            "SELECT meeting_audio_chunks.seq FROM meeting_audio_chunks WHERE "
            "meeting_audio_chunks.meeting_id = ? AND meeting_audio_chunks.channel = ? ORDER BY "
            "meeting_audio_chunks.seq DESC LIMIT 1");
        detail::bind_all(statement, {std::string(meeting_id), std::string(channel)});
        return statement.step() ? detail::int_value(statement.column(0)) + 1 : std::int64_t{0};
    });
}

std::int64_t SqlMeetingRepository::mark_chunks_done(std::string_view meeting_id) {
    // After a successful offline replace, so unfinished live chunks cannot resurrect the draft.
    return database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        const auto rows = unfinished(connection, meeting_id);
        for (const auto& [id, row] : rows) {
            exec_bound(connection,
                       "UPDATE meeting_audio_chunks SET asr_status = 'done', asr_error = NULL "
                       "WHERE meeting_audio_chunks.id = ?",
                       {id});
        }
        transaction.commit();
        return static_cast<std::int64_t>(rows.size());
    });
}

} // namespace fv::meeting_state
