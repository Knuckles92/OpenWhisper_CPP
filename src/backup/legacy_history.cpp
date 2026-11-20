#include "backup/legacy_history.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "backup/backup.h"
#include "backup/database_sanitizer.h"
#include "backup/detail/fs.h"
#include "backup/detail/values.h"
#include "backup/names.h"
#include "db/schema.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "sql/db.h"

namespace fv::backup {

namespace {

namespace th = schema::transcription_history;

// The columns a legacy entry may fill, in this order.
constexpr std::array<std::string_view, 9> kAllowed = {
    th::id,       th::text,       th::raw_text,           th::timestamp,
    th::model,    th::audio_file, th::transcription_time, th::audio_duration,
    th::file_size};

// Python's truth value of a JSON value.
bool truthy(const Json& value) {
    if (value.is_null()) {
        return false;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_float()) {
        return value.get<double>() != 0.0;
    }
    if (value.is_number()) {
        return value != 0;
    }
    if (value.is_string() || value.is_array() || value.is_object()) {
        return !value.empty();
    }
    return true;
}

Json load_entries(const std::filesystem::path& legacy) {
    Json data;
    try {
        const std::string bytes = detail::read_file(legacy);
        if (!detail::is_valid_utf8(bytes)) {
            throw BackupError("Legacy history JSON is invalid");
        }
        data = py::loads(bytes);
    } catch (const std::filesystem::filesystem_error&) {
        throw BackupError("Legacy history JSON is invalid");
    } catch (const py::ValueError&) {
        throw BackupError("Legacy history JSON is invalid");
    }
    const Json* entries = nullptr;
    if (data.is_object()) {
        if (const auto it = data.find("entries"); it != data.end()) {
            entries = &*it;
        }
    }
    if (entries == nullptr || !entries->is_array()) {
        throw BackupError("Legacy history JSON has no valid entries list");
    }
    return *entries;
}

void insert_entries(sql::Connection& connection, const Json& entries) {
    const std::set<std::string> table_columns = columns(connection, th::table);
    for (const auto required : {th::id, th::text, th::timestamp, th::model}) {
        if (!table_columns.contains(std::string(required))) {
            throw BackupError("Cannot merge legacy history into this database schema");
        }
    }
    for (const auto& entry : entries) {
        if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string()) {
            throw BackupError("Legacy history entry is invalid");
        }
        std::vector<std::pair<std::string, Json>> values;
        for (const auto key : kAllowed) {
            if (!table_columns.contains(std::string(key))) {
                continue;
            }
            const auto it = entry.find(std::string(key));
            Json value = it == entry.end() ? Json() : *it;
            if ((key == th::text || key == th::timestamp || key == th::model) && !truthy(value)) {
                value = "";
            }
            values.emplace_back(std::string(key), std::move(value));
        }
        std::string names;
        std::string marks;
        for (const auto& [name, value] : values) {
            names += (names.empty() ? "" : ",") + name;
            marks += marks.empty() ? "?" : ",?";
        }
        sql::Statement statement = connection.prepare(
            "INSERT OR REPLACE INTO transcription_history (" + names + ") VALUES (" + marks + ")");
        for (std::size_t i = 0; i < values.size(); ++i) {
            const int index = static_cast<int>(i + 1);
            statement.bind(index, detail::values::from_json(values[i].second, index));
        }
        while (statement.step()) {
        }
    }
}

} // namespace

void merge_legacy_history(const std::filesystem::path& db,
                          const std::filesystem::path& source_root) {
    const std::filesystem::path legacy = source_root / kLegacyHistoryFile;
    if (!detail::exists(legacy)) {
        return;
    }
    if (detail::is_link(legacy) || !detail::is_file(legacy) ||
        detail::stat(legacy).size > kMaxManifestBytes) {
        throw BackupError("Legacy history JSON is unsafe or too large");
    }
    sql::Connection connection = sql::Connection::open(db, sql::OpenMode::ReadWrite);
    if (!has_table(connection, th::table)) {
        throw BackupError("Cannot merge legacy history into this database schema");
    }
    if (connection.prepare("SELECT 1 FROM transcription_history LIMIT 1").step()) {
        return; // the condition of the database's own first-run migration
    }
    const Json entries = load_entries(legacy);
    sql::Transaction transaction = connection.begin();
    insert_entries(connection, entries);
    transaction.commit();
}

} // namespace fv::backup
