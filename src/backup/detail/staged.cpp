#include "backup/detail/staged.h"

#include <cstdint>
#include <system_error>
#include <vector>

#include "backup/backup.h"
#include "backup/database_sanitizer.h"
#include "backup/detail/archive_check.h"
#include "backup/detail/fs.h"
#include "backup/detail/json_file.h"
#include "backup/members.h"
#include "base/hash.h"
#include "base/utf8.h"
#include "db/schema.h"
#include "pyport/json.h"

namespace fv::backup::detail {

namespace {

const Json* get(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

// A Python int that is not a bool and not below zero.
bool is_size(const Json* value) {
    if (value == nullptr || !value->is_number_integer()) {
        return false;
    }
    return value->is_number_unsigned() || value->get<std::int64_t>() >= 0;
}

void walk(const std::filesystem::path& folder, const std::string& relative,
          std::set<std::string>& found) {
    std::error_code error;
    std::filesystem::directory_iterator it(folder, error);
    if (error) {
        return; // os.walk skips a folder it cannot list
    }
    std::vector<std::pair<std::string, std::filesystem::path>> dirs;
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    for (const std::filesystem::directory_iterator end; it != end; it.increment(error)) {
        if (error) {
            break;
        }
        std::error_code type_error;
        const bool is_dir = it->is_directory(type_error) && !type_error;
        (is_dir ? dirs : files).emplace_back(path_to_utf8(it->path().filename()), it->path());
    }
    for (const auto& [name, path] : dirs) {
        if (is_link(path)) {
            throw BackupError("Staged restore has a symbolic link");
        }
    }
    for (const auto& [name, path] : files) {
        if (is_link(path) || !stat(path).regular) {
            throw BackupError("Staged restore has a special file");
        }
        found.insert("data/" + relative + name);
    }
    for (const auto& [name, path] : dirs) {
        walk(path, relative + name + "/", found);
    }
}

} // namespace

std::string file_sha256(const std::filesystem::path& path) {
    InputFile input(path);
    Sha256 digest;
    std::vector<std::byte> buffer(kChunkSize);
    while (const std::size_t got = input.read(buffer)) {
        digest.update(std::span<const std::byte>(buffer.data(), got));
    }
    return digest.hexdigest();
}

std::string settings_text(const Json& settings) {
    py::DumpOptions options;
    options.indent = 2;
    options.ensure_ascii = false;
    const std::string text = py::dumps(settings, options) + "\n";
#ifdef _WIN32
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char c : text) {
        if (c == '\n') {
            out += '\r';
        }
        out += c;
    }
    return out;
#else
    return text;
#endif
}

void check_staged_file(const std::filesystem::path& stage, const Json& manifest, const Json& item,
                       std::set<std::string>& listed, const Names& names) {
    static const Json kNone;
    if (!item.is_object()) {
        throw BackupError("Staged restore manifest is invalid");
    }
    const Json* path = get(item, "path");
    const std::string name = safe_member(path != nullptr ? *path : kNone, names);
    if (name == kManifestName) {
        throw BackupError("Staged restore manifest is invalid");
    }
    if (listed.contains(name)) {
        throw BackupError("Staged restore has duplicate entries");
    }
    listed.insert(name);
    const std::filesystem::path source =
        safe_path(stage / "data", std::string_view(name).substr(5), names);
    const Json* expected_size = nullptr;
    const Json* expected_hash = nullptr;
    if (name == names.database_member) {
        expected_size = get(manifest, "staged_db_size");
        expected_hash = get(manifest, "staged_db_sha256");
    } else if (name == names.settings_member) {
        expected_size = get(manifest, "staged_settings_size");
        expected_hash = get(manifest, "staged_settings_sha256");
    } else {
        expected_size = get(item, "size");
        expected_hash = get(item, "sha256");
    }
    if (!is_size(expected_size) || !is_file(source) ||
        stat(source).size != expected_size->get<std::uint64_t>()) {
        throw BackupError("Staged restore file is missing or changed");
    }
    if (expected_hash == nullptr || !expected_hash->is_string() ||
        file_sha256(source) != expected_hash->get_ref<const std::string&>()) {
        throw BackupError("Staged restore checksum mismatch");
    }
}

std::set<std::string> staged_tree_files(const std::filesystem::path& data_stage) {
    std::set<std::string> found;
    walk(data_stage, "", found);
    return found;
}

void check_staged_meeting_paths(sql::Connection& connection, const std::filesystem::path& root,
                                const std::set<std::string>& found, const Names& names) {
    static constexpr std::pair<std::string_view, std::string_view> kColumns[] = {
        {schema::meeting_sessions::table, schema::meeting_sessions::spool_dir},
        {schema::meeting_audio_chunks::table, schema::meeting_audio_chunks::file_path}};
    for (const auto& [table, column] : kColumns) {
        if (!has_table(connection, table) ||
            !columns(connection, table).contains(std::string(column))) {
            continue;
        }
        sql::Statement statement =
            connection.prepare("SELECT " + std::string(column) + " FROM " + std::string(table));
        while (statement.step()) {
            const sql::Value value = statement.column(0);
            const auto* text = std::get_if<std::string>(&value);
            if (text == nullptr) {
                throw BackupError("Staged database has an unsafe path");
            }
            const auto parts = relative_parts(path_from_utf8(*text), root);
            if (!parts) {
                throw BackupError("Staged database path escapes data root");
            }
            std::string relative;
            for (const auto& part : *parts) {
                relative += (relative.empty() ? "" : "/") + part;
            }
            if (!relative.starts_with("meetings/")) {
                throw BackupError("Staged database path is outside meetings");
            }
            safe_member("data/" + relative, names);
            if (table == schema::meeting_audio_chunks::table &&
                !found.contains("data/" + relative)) {
                throw BackupError("Staged database references missing meeting audio");
            }
        }
    }
}

void restore_stage_check(const std::filesystem::path& stage, const std::filesystem::path& root,
                         const Names& names) {
    const Json manifest = read_json(stage / path_from_utf8(kManifestName));
    const Json* format = get(manifest, "format");
    const Json* version = get(manifest, "format_version");
    const Json* schema_version = get(manifest, "schema_version");
    const Json* include_recordings = get(manifest, "include_recordings");
    // isinstance(value, int) is true of a bool too.
    if (format == nullptr || !format->is_string() || *format != names.format ||
        version == nullptr || !py::py_equal(*version, Json(kFormatVersion)) ||
        schema_version == nullptr ||
        !(schema_version->is_number_integer() || schema_version->is_boolean()) ||
        include_recordings == nullptr || !include_recordings->is_boolean()) {
        throw BackupError("Staged restore manifest is incompatible");
    }
    const Json* files = get(manifest, "files");
    if (files == nullptr || !files->is_array() || files->size() > kMaxFiles) {
        throw BackupError("Staged restore manifest is invalid");
    }
    const std::filesystem::path data_stage = stage / "data";
    if (is_link(data_stage)) {
        throw BackupError("Staged data directory is a symbolic link");
    }
    std::set<std::string> listed;
    for (const auto& item : *files) {
        check_staged_file(stage, manifest, item, listed, names);
    }
    const std::set<std::string> found = staged_tree_files(data_stage);
    if (found != listed) {
        throw BackupError("Staged restore contains unlisted data");
    }
    sql::Connection connection = sql::Connection::open(
        data_stage / path_from_utf8(names.database_file), sql::OpenMode::Create);
    quick_check(connection);
    if (!py::py_equal(Json(db_schema(connection)), *schema_version) ||
        meeting_is_live(connection)) {
        throw BackupError("Staged database metadata is invalid");
    }
    check_staged_meeting_paths(connection, root, found, names);
    check_history_audio(connection, found, "Staged database has an unsafe recording path",
                        "Staged database references missing recording audio", names);
}

} // namespace fv::backup::detail
