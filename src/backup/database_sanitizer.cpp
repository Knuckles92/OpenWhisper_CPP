#include "backup/database_sanitizer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>

#include "backup/backup.h"
#include "backup/detail/fs.h"
#include "backup/detail/values.h"
#include "backup/members.h"
#include "base/ids.h"
#include "base/utf8.h"
#include "db/database.h"
#include "db/schema.h"
#include "pyport/unicode.h"

namespace fv::backup {

namespace {

namespace ms = schema::meeting_sessions;
namespace mac = schema::meeting_audio_chunks;
namespace th = schema::transcription_history;

// Meeting columns given a fresh random value, then ones cleared, in that order.
constexpr std::array<std::string_view, 2> kMeetingTokenColumns = {ms::host_token, ms::guest_token};
constexpr std::array<std::string_view, 4> kMeetingClearedColumns = {
    ms::asr_remote_json, ms::agent_endpoint_json, ms::app_pid, ms::app_heartbeat_at};

// sqlite3.connect(source, timeout=30) for the snapshot's reader.
constexpr std::chrono::seconds kSnapshotBusyTimeout{30};

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

// root / PurePath(text), as pathlib joins: a path on root's drive without a root of its own is
// taken below root, whatever the drive letter's case.
std::filesystem::path join_pure(const std::filesystem::path& root,
                                const std::filesystem::path& path) {
    if (kWindows && path.has_root_name() && !path.has_root_directory() &&
        detail::values::fold_case(path_to_utf8(path.root_name())) ==
            detail::values::fold_case(path_to_utf8(root.root_name()))) {
        return root / path.relative_path();
    }
    return root / path;
}

// ntpath.splitroot finds a drive in a part whose second character is ":".
bool starts_like_drive(const std::string& part) {
    std::size_t at = 0;
    if (part.empty()) {
        return false;
    }
    py::next_code_point(part, at);
    return at < part.size() && part[at] == ':';
}

void exec_update(sql::Connection& connection, std::string_view sql,
                 const std::vector<sql::Value>& values) {
    sql::Statement statement = connection.prepare(sql);
    for (std::size_t i = 0; i < values.size(); ++i) {
        statement.bind(static_cast<int>(i + 1), values[i]);
    }
    while (statement.step()) {
    }
}

// _DatabaseSanitizer.
class Sanitizer {
public:
    Sanitizer(sql::Connection& connection, const std::filesystem::path& source_root,
              bool include_recordings, const Names& names)
        : conn_(connection), root_(source_root), include_recordings_(include_recordings),
          names_(names) {}

    void meeting_sessions() {
        if (!has_table(conn_, ms::table)) {
            return;
        }
        const std::set<std::string> cols = columns(conn_, ms::table);
        if (!cols.contains(std::string(ms::id)) || !cols.contains(std::string(ms::spool_dir))) {
            return;
        }
        const auto rows = select(conn_, "SELECT id, spool_dir FROM meeting_sessions");
        for (const auto& row : rows) {
            const sql::Value& meeting_id = row[0];
            std::optional<std::string> rel =
                relative_under(detail::values::str_or_empty(row[1]), root_, "meetings", names_);
            if (!rel) {
                if (include_recordings_) {
                    throw BackupError("A meeting stores audio outside the data directory");
                }
                rel = "meetings/" + detail::values::str(meeting_id);
            }
            const auto updates = meeting_session_updates(cols, *rel);
            std::string sql = "UPDATE meeting_sessions SET ";
            std::vector<sql::Value> values;
            for (std::size_t i = 0; i < updates.size(); ++i) {
                sql += (i == 0 ? "" : ", ") + updates[i].first + "=?";
                values.push_back(updates[i].second);
            }
            sql += " WHERE id=?";
            values.push_back(meeting_id);
            exec_update(conn_, sql, values);
        }
    }

    void meeting_audio_chunks() {
        if (!has_table(conn_, mac::table)) {
            return;
        }
        if (!include_recordings_) {
            if (has_table(conn_, schema::meeting_segments::table) &&
                columns(conn_, schema::meeting_segments::table).contains("chunk_id")) {
                conn_.exec("UPDATE meeting_segments SET chunk_id=NULL");
            }
            conn_.exec("DELETE FROM meeting_audio_chunks");
            return;
        }
        const auto chunks = select(conn_, "SELECT id, file_path FROM meeting_audio_chunks");
        for (const auto& chunk : chunks) {
            const sql::Value& chunk_id = chunk[0];
            const std::optional<std::string> rel =
                relative_under(detail::values::str_or_empty(chunk[1]), root_, "meetings", names_);
            if (!rel) {
                throw BackupError("A meeting chunk points outside the data directory");
            }
            if (!detail::is_file(detail::join_parts(root_, *rel))) {
                throw BackupError("Meeting chunk " + detail::values::str(chunk_id) +
                                  " has no local audio file");
            }
            required_.insert("data/" + *rel);
            exec_update(conn_, "UPDATE meeting_audio_chunks SET file_path=? WHERE id=?",
                        {sql::Value(std::string(kPathMarker) + *rel), chunk_id});
        }
    }

    void history_audio() {
        if (!has_table(conn_, th::table) ||
            !columns(conn_, th::table).contains(std::string(th::audio_file))) {
            return;
        }
        if (!include_recordings_) {
            conn_.exec("UPDATE transcription_history SET audio_file=NULL");
            return;
        }
        const auto rows = select(
            conn_, "SELECT id, audio_file FROM transcription_history WHERE audio_file IS NOT NULL");
        for (const auto& row : rows) {
            const sql::Value& row_id = row[0];
            const auto* audio = std::get_if<std::string>(&row[1]);
            if (audio == nullptr) {
                clear_audio(row_id);
            } else if (detail::is_absolute_text(*audio)) {
                absolute_history_audio(row_id, *audio);
            } else {
                relative_history_audio(row_id, *audio);
            }
        }
    }

    std::vector<std::string> warnings;

    const std::set<std::string>& required_assets() const { return required_; }

private:
    static std::vector<std::vector<sql::Value>> select(sql::Connection& connection,
                                                       std::string_view sql) {
        std::vector<std::vector<sql::Value>> rows;
        sql::Statement statement = connection.prepare(sql);
        while (statement.step()) {
            std::vector<sql::Value> row;
            for (int i = 0; i < statement.column_count(); ++i) {
                row.push_back(statement.column(i));
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

    void absolute_history_audio(const sql::Value& row_id, const std::string& audio) {
        const std::optional<std::string> rel = relative_under(audio, root_, "recordings", names_);
        if (!rel) {
            warnings.push_back("History entry " + detail::values::str(row_id) +
                               " refers to external audio");
            clear_audio(row_id);
        } else if (detail::is_file(detail::join_parts(root_, *rel))) {
            required_.insert("data/" + *rel);
            exec_update(conn_, "UPDATE transcription_history SET audio_file=? WHERE id=?",
                        {sql::Value(rel->substr(std::string_view("recordings/").size())), row_id});
        } else {
            missing_history_audio(row_id);
        }
    }

    void relative_history_audio(const sql::Value& row_id, const std::string& audio) {
        try {
            safe_member("data/recordings/" + audio, names_);
        } catch (const BackupError&) {
            clear_audio(row_id);
            return;
        }
        const std::string rel = "recordings/" + audio;
        if (detail::is_file(detail::join_parts(root_, rel))) {
            required_.insert("data/" + rel);
        } else {
            missing_history_audio(row_id);
        }
    }

    void missing_history_audio(const sql::Value& row_id) {
        warnings.push_back("History entry " + detail::values::str(row_id) +
                           " has no local audio file");
        clear_audio(row_id);
    }

    void clear_audio(const sql::Value& row_id) {
        exec_update(conn_, "UPDATE transcription_history SET audio_file=NULL WHERE id=?", {row_id});
    }

    sql::Connection& conn_;
    std::filesystem::path root_;
    bool include_recordings_;
    const Names& names_;
    std::set<std::string> required_;
};

} // namespace

bool has_table(sql::Connection& connection, std::string_view table) {
    sql::Statement statement =
        connection.prepare("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    statement.bind(1, sql::Value(std::string(table)));
    return statement.step();
}

std::set<std::string> columns(sql::Connection& connection, std::string_view table) {
    std::set<std::string> names;
    sql::Statement statement = connection.prepare("PRAGMA table_info(" + std::string(table) + ")");
    while (statement.step()) {
        names.insert(detail::values::str(statement.column(1)));
    }
    return names;
}

int db_schema(sql::Connection& connection) {
    if (!has_table(connection, schema::schema_version_table::table)) {
        return 0;
    }
    sql::Statement statement = connection.prepare("SELECT version FROM schema_version LIMIT 1");
    if (!statement.step()) {
        return 0;
    }
    return detail::values::to_int(statement.column(0));
}

void quick_check(sql::Connection& connection) {
    const sql::Value result = connection.scalar("PRAGMA quick_check");
    const auto* text = std::get_if<std::string>(&result);
    if (text == nullptr || *text != "ok") {
        throw BackupError("Backup database failed SQLite integrity check");
    }
    sql::Statement statement = connection.prepare("PRAGMA foreign_key_check");
    if (statement.step()) {
        throw BackupError("Backup database has broken foreign keys");
    }
}

bool meeting_is_live(sql::Connection& connection) {
    if (!has_table(connection, ms::table) ||
        !columns(connection, ms::table).contains(std::string(ms::status))) {
        return false;
    }
    sql::Statement statement = connection.prepare(
        "SELECT 1 FROM meeting_sessions WHERE status IN ('active','paused') LIMIT 1");
    return statement.step();
}

int snapshot_database(const std::filesystem::path& source, const std::filesystem::path& output,
                      const CancelToken& cancel) {
    if (detail::exists(source)) {
        if (detail::is_link(source) || !detail::is_file(source)) {
            throw BackupError("Database path is unsafe");
        }
        sql::Connection read = sql::Connection::open(source, sql::OpenMode::ReadOnly);
        read.set_busy_timeout(kSnapshotBusyTimeout);
        if (meeting_is_live(read)) {
            throw BackupError("End the live meeting before creating a backup");
        }
        sql::Connection write = sql::Connection::open(output, sql::OpenMode::Create);
        try {
            read.backup_to(write, cancel);
        } catch (const sql::Error&) {
            if (cancel.cancelled()) {
                throw BackupCancelled();
            }
            throw;
        }
    } else {
        // A legacy-only data root still needs a complete current schema, so that imported
        // history survives the next database start-up.
        Database::open_or_create(output).close();
    }
    sql::Connection connection = sql::Connection::open(output, sql::OpenMode::ReadWrite);
    quick_check(connection);
    return db_schema(connection);
}

std::optional<std::string> relative_under(std::string_view path_text,
                                          const std::filesystem::path& root, std::string_view group,
                                          const Names& names) {
    std::filesystem::path path = path_from_utf8(path_text);
    if (!path.is_absolute()) {
        path = join_pure(root, path);
    }
    const auto parts = detail::relative_parts(path, root);
    if (!parts || parts->empty() || (*parts)[0] != group) {
        return std::nullopt;
    }
    if (kWindows && std::ranges::any_of(*parts, starts_like_drive)) {
        // relative_to rebuilds the result by joining its parts, and ntpath reads "x:..." as a
        // drive: the result's first part is then that drive, never `group`.
        return std::nullopt;
    }
    std::string value;
    for (const auto& part : *parts) {
        value += (value.empty() ? "" : "/") + part;
    }
    safe_member("data/" + value, names);
    return value;
}

std::vector<std::pair<std::string, sql::Value>>
meeting_session_updates(const std::set<std::string>& table_columns, std::string_view rel) {
    std::vector<std::pair<std::string, sql::Value>> updates;
    updates.emplace_back(std::string(ms::spool_dir), std::string(kPathMarker) + std::string(rel));
    for (const auto column : kMeetingTokenColumns) {
        if (table_columns.contains(std::string(column))) {
            updates.emplace_back(std::string(column), token_urlsafe(32));
        }
    }
    for (const auto column : kMeetingClearedColumns) {
        if (table_columns.contains(std::string(column))) {
            updates.emplace_back(std::string(column), nullptr);
        }
    }
    return updates;
}

SanitizedDatabase sanitize_database(const std::filesystem::path& path,
                                    const std::filesystem::path& source_root,
                                    bool include_recordings, const Names& names) {
    sql::Connection connection = sql::Connection::open(path, sql::OpenMode::ReadWrite);
    connection.exec("PRAGMA journal_mode=DELETE");
    SanitizedDatabase result;
    result.schema_version = db_schema(connection);
    if (result.schema_version > schema::schema_version) {
        throw BackupError("Database schema is newer than this app");
    }
    Sanitizer sanitizer(connection, source_root, include_recordings, names);
    sql::Transaction transaction = connection.begin();
    sanitizer.meeting_sessions();
    sanitizer.meeting_audio_chunks();
    sanitizer.history_audio();
    if (has_table(connection, schema::record_sync::table)) {
        connection.exec("DELETE FROM record_sync");
    }
    transaction.commit();
    quick_check(connection);
    result.warnings = std::move(sanitizer.warnings);
    result.required_assets = sanitizer.required_assets();
    return result;
}

} // namespace fv::backup
