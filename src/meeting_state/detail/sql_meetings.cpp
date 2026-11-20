// SqlMeetingRepository: meeting rows (repository.py create_meeting .. find_interrupted_meetings).

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <utility>

#include "base/log.h"
#include "db/database.h"
#include "history/titles.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/sql_internal.h"
#include "meeting_state/schema.h"
#include "meeting_state/sql_repository.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.persist.repository");

// MeetingSession's columns in table order.
constexpr std::array<std::string_view, 21> kColumns = {"id",
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

bool is_column(std::string_view name) {
    return std::find(kColumns.begin(), kColumns.end(), name) != kColumns.end();
}

// The ORM's Python-side column defaults.
Json column_default(std::string_view column) {
    if (column == "title") {
        return "";
    }
    if (column == "paused_total_s") {
        return 0.0;
    }
    if (column == "cloud_enabled") {
        return false;
    }
    if (column == "state_seq") {
        return 0;
    }
    return Json();
}

std::optional<Json> select_meeting(sql::Connection& connection, std::string_view meeting_id) {
    sql::Statement statement = connection.prepare("SELECT " + std::string(detail::kSessionColumns) +
                                                  " FROM meeting_sessions WHERE "
                                                  "meeting_sessions.id = ?");
    statement.bind(1, std::string(meeting_id));
    if (!statement.step()) {
        return std::nullopt;
    }
    return detail::session_dict(statement);
}

std::vector<Json> select_meetings(sql::Connection& connection, const std::string& sql) {
    sql::Statement statement = connection.prepare(sql);
    std::vector<Json> rows;
    while (statement.step()) {
        rows.push_back(detail::session_dict(statement));
    }
    return rows;
}

} // namespace

void SqlMeetingRepository::create_meeting(const Json& fields) {
    if (!fields.is_object()) {
        throw py::TypeError("create_meeting() takes keyword arguments");
    }
    std::string names;
    std::vector<sql::Value> values;
    for (const auto& [key, value] : fields.items()) {
        if (!is_column(key)) {
            throw py::TypeError("'" + key + "' is an invalid keyword argument for MeetingSession");
        }
    }
    for (const std::string_view column : kColumns) {
        const auto given = fields.find(std::string(column));
        Json value = given != fields.end() ? *given : column_default(column);
        if (given == fields.end() && value.is_null()) {
            continue;
        }
        names += names.empty() ? "" : ", ";
        names += column;
        values.push_back(detail::to_value(value));
    }
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        sql::Statement statement =
            connection.prepare("INSERT INTO meeting_sessions (" + names + ") VALUES (" +
                               detail::placeholders(values.size()) + ")");
        detail::bind_all(statement, values);
        statement.step();
        transaction.commit();
    });
}

void SqlMeetingRepository::update_meeting(std::string_view meeting_id, const Json& fields) {
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        if (!select_meeting(connection, meeting_id)) {
            logger.warning("update_meeting: unknown meeting {}", meeting_id);
            return;
        }
        std::string assignments;
        std::vector<sql::Value> values;
        if (fields.is_object()) {
            for (const auto& [key, value] : fields.items()) {
                // setattr of something that is not a column persists nothing.
                if (!is_column(key)) {
                    continue;
                }
                assignments += assignments.empty() ? "" : ", ";
                assignments += key + " = ?";
                values.push_back(detail::to_value(value));
            }
        }
        if (!assignments.empty()) {
            values.emplace_back(std::string(meeting_id));
            sql::Statement statement = connection.prepare(
                "UPDATE meeting_sessions SET " + assignments + " WHERE meeting_sessions.id = ?");
            detail::bind_all(statement, values);
            statement.step();
        }
        transaction.commit();
    });
}

void SqlMeetingRepository::rename_meeting(std::string_view meeting_id, std::string_view title) {
    const std::string clean_title = normalize_title(title);
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin(sql::TxMode::Immediate);
        const std::optional<Json> row = select_meeting(connection, meeting_id);
        if (!row) {
            throw py::ValueError("unknown meeting '" + std::string(meeting_id) + "'");
        }
        const Json& raw = row->at("state_json");
        Json state = parse_state_json(raw).value_or(Json());
        if (state.is_null()) {
            if (detail::truthy(raw)) {
                logger.warning("Replacing corrupt state for meeting {}", meeting_id);
            }
            state = Json::object();
        }
        state["meeting_id"] = meeting_id;
        state["title"] = clean_title;
        sql::Statement statement = connection.prepare(
            "UPDATE meeting_sessions SET title = ?, state_json = ? WHERE meeting_sessions.id = ?");
        detail::bind_all(statement, {clean_title, py::dumps(state, {.ensure_ascii = false}),
                                     std::string(meeting_id)});
        statement.step();
        transaction.commit();
    });
}

void SqlMeetingRepository::replace_tokens(std::string_view meeting_id, std::string_view host_token,
                                          std::string_view guest_token) {
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        if (!select_meeting(connection, meeting_id)) {
            throw py::ValueError("unknown meeting '" + std::string(meeting_id) + "'");
        }
        sql::Statement statement =
            connection.prepare("UPDATE meeting_sessions SET host_token = ?, guest_token = ? "
                               "WHERE meeting_sessions.id = ?");
        detail::bind_all(statement, {std::string(host_token), std::string(guest_token),
                                     std::string(meeting_id)});
        statement.step();
        transaction.commit();
    });
}

std::optional<Json> SqlMeetingRepository::get_meeting(std::string_view meeting_id) {
    return database_.read(
        [&](sql::Connection& connection) { return select_meeting(connection, meeting_id); });
}

std::vector<Json> SqlMeetingRepository::list_meetings() {
    return database_.read([&](sql::Connection& connection) {
        return select_meetings(connection, "SELECT " + std::string(detail::kSessionColumns) +
                                               " FROM meeting_sessions ORDER BY "
                                               "meeting_sessions.started_at DESC");
    });
}

void SqlMeetingRepository::delete_meeting(std::string_view meeting_id) {
    std::function<void()> count_features;
    {
        std::lock_guard lock(mutex_);
        count_features = count_features_;
    }
    // Stats keep counting a meeting after it is deleted, so it needs its row before its
    // transcript goes.
    try {
        if (count_features) {
            count_features();
        }
    } catch (const std::exception& error) {
        logger.debug("Could not count meetings for Stats: {}", error.what());
    }
    database_.write([&](sql::Connection& connection) {
        sql::Transaction transaction = connection.begin();
        // Explicit deletes: SQLite does not fire the FTS sync triggers for cascade deletes, which
        // would orphan transcript text in the search index.
        for (const std::string_view table :
             {"meeting_events", "meeting_questions", "meeting_state_items", "meeting_participants",
              "meeting_segments", "meeting_audio_chunks"}) {
            sql::Statement statement =
                connection.prepare("DELETE FROM " + std::string(table) + " WHERE " +
                                   std::string(table) + ".meeting_id = ?");
            statement.bind(1, std::string(meeting_id));
            statement.step();
        }
        sql::Statement statement =
            connection.prepare("DELETE FROM meeting_sessions WHERE meeting_sessions.id = ?");
        statement.bind(1, std::string(meeting_id));
        statement.step();
        transaction.commit();
    });
}

void SqlMeetingRepository::heartbeat(std::string_view meeting_id, std::int64_t pid,
                                     std::string_view utc_iso) {
    Json fields = Json::object();
    fields["app_pid"] = pid;
    fields["app_heartbeat_at"] = utc_iso;
    update_meeting(meeting_id, fields);
}

std::vector<Json> SqlMeetingRepository::find_interrupted_meetings() {
    return database_.read([&](sql::Connection& connection) {
        return select_meetings(
            connection,
            "SELECT " + std::string(detail::kSessionColumns) +
                " FROM meeting_sessions WHERE meeting_sessions.status IN "
                "('active', 'paused', 'ending', 'needs_recovery') OR meeting_sessions.id IN "
                "(SELECT meeting_audio_chunks.meeting_id FROM meeting_audio_chunks WHERE "
                "meeting_audio_chunks.asr_status != 'done')");
    });
}

} // namespace fv::meeting_state
