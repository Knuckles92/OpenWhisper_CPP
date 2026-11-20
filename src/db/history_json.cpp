#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

#include "base/json.h"
#include "base/log.h"
#include "base/utf8.h"
#include "db/detail/startup.h"
#include "pyport/json.h"
#include "sql/db.h"

namespace fv::db_detail {

namespace {

const log::Logger logger{"services.database"};

// session.merge of TranscriptionHistory(id, text, timestamp, model, audio_file,
// transcription_time, audio_duration, file_size): an upsert by id of those eight columns.
constexpr std::string_view merge_sql =
    "INSERT INTO transcription_history (id, text, timestamp, model, audio_file, "
    "transcription_time, audio_duration, file_size) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(id) DO UPDATE SET text = excluded.text, timestamp = excluded.timestamp, "
    "model = excluded.model, audio_file = excluded.audio_file, "
    "transcription_time = excluded.transcription_time, "
    "audio_duration = excluded.audio_duration, file_size = excluded.file_size";

// Python's truth value of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return value.get<std::int64_t>() != 0;
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_string()) {
        return !value.get_ref<const std::string&>().empty();
    }
    return !value.empty();
}

// What the sqlite3 module binds for a JSON value; a list or a dict cannot be bound.
sql::Value bindable(const Json& value) {
    if (value.is_null()) {
        return nullptr;
    }
    if (value.is_boolean()) {
        return std::int64_t{value.get<bool>() ? 1 : 0};
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(INT64_MAX)) {
            throw std::overflow_error("Python int too large to convert to SQLite INTEGER");
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    throw std::invalid_argument("Error binding parameter: type is not supported");
}

// entry.get(key, fallback) of a dict; an entry that is not a dict has no .get.
const Json& get(const Json& entry, std::string_view key, const Json& fallback) {
    const auto found = entry.find(key);
    return found == entry.end() ? fallback : *found;
}

void migrate_history_from_json(sql::Connection& conn, const std::filesystem::path& json_path) {
    try {
        std::ifstream file(json_path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("cannot open " + path_to_utf8(json_path));
        }
        const std::string text{std::istreambuf_iterator<char>(file),
                               std::istreambuf_iterator<char>()};
        // Closed before the rename below: Windows cannot rename an open file.
        file.close();
        const Json data = py::loads(text);
        if (!data.is_object()) {
            throw std::runtime_error("the history file does not hold a JSON object");
        }
        const Json none = Json::array();
        const Json& entries = get(data, "entries", none);
        if (!truthy(entries)) {
            logger.info("No history entries to migrate");
            return;
        }
        if (!entries.is_array()) {
            throw std::runtime_error("the history entries are not a list");
        }

        const Json empty = "";
        const Json null = nullptr;
        sql::Transaction transaction = conn.begin();
        sql::Statement merge = conn.prepare(merge_sql);
        for (const Json& entry : entries) {
            if (!entry.is_object()) {
                throw std::runtime_error("a history entry is not a JSON object");
            }
            merge.reset();
            merge.bind(1, bindable(get(entry, "id", null)));
            merge.bind(2, bindable(get(entry, "text", empty)));
            merge.bind(3, bindable(get(entry, "timestamp", empty)));
            merge.bind(4, bindable(get(entry, "model", empty)));
            merge.bind(5, bindable(get(entry, "audio_file", null)));
            merge.bind(6, bindable(get(entry, "transcription_time", null)));
            merge.bind(7, bindable(get(entry, "audio_duration", null)));
            merge.bind(8, bindable(get(entry, "file_size", null)));
            merge.step();
        }
        merge.reset();
        transaction.commit();

        const std::string backup_text = path_to_utf8(json_path) + ".bak";
        const std::filesystem::path backup_path = path_from_utf8(backup_text);
#ifdef _WIN32
        // os.rename on Windows refuses an existing target; std::filesystem::rename replaces it.
        if (std::filesystem::exists(backup_path)) {
            throw std::runtime_error("[WinError 183] Cannot create a file when that file already "
                                     "exists: '" +
                                     path_to_utf8(json_path) + "' -> '" + backup_text + "'");
        }
#endif
        std::filesystem::rename(json_path, backup_path);
        logger.info("Migrated {} history entries from JSON. Backup: {}", entries.size(),
                    backup_text);
    } catch (const std::exception& error) {
        logger.error_with(error,
                          std::string("Failed to migrate history from JSON: ") + error.what());
    }
}

} // namespace

void migrate_from_json(sql::Connection& conn, const std::filesystem::path& history_file) {
    const sql::Value count =
        conn.scalar("SELECT count(transcription_history.id) FROM transcription_history");
    const auto* history_count = std::get_if<std::int64_t>(&count);
    std::error_code error;
    if (std::filesystem::exists(history_file, error) && history_count != nullptr &&
        *history_count == 0) {
        migrate_history_from_json(conn, history_file);
    }
}

} // namespace fv::db_detail
