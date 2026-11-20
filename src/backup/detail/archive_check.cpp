#include "backup/detail/archive_check.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "archive/errors.h"
#include "backup/backup.h"
#include "backup/database_sanitizer.h"
#include "backup/detail/fs.h"
#include "backup/detail/json_bytes.h"
#include "backup/members.h"
#include "base/hash.h"
#include "db/schema.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::backup::detail {

namespace {

using Members = std::map<std::string, const archive::EntryInfo*>;

constexpr std::uint32_t kFormatMask = 0170000; // stat.S_IFMT
constexpr std::uint32_t kRegular = 0100000;    // stat.S_IFREG
constexpr std::size_t kMaxWarningChars = 500;

// A Python int that is not a bool.
bool is_int(const Json& value) {
    return value.is_number_integer();
}

const Json* get(const Json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

// _check_member_entry.
void check_member_entry(const archive::EntryInfo& info) {
    const std::uint32_t mode = info.external_attr >> 16;
    if (mode != 0 && (mode & kFormatMask) != 0 && (mode & kFormatMask) != kRegular) {
        throw BackupError("Archive contains a symbolic link or special file");
    }
    if (info.size > kMaxFileBytes) {
        throw BackupError("Archive member exceeds size limit");
    }
    // size / compress_size > MAX_RATIO; size is at most 64 GiB here, so the product cannot
    // overflow once compress_size is below size.
    if (info.size != 0 &&
        (info.compressed_size == 0 ||
         (info.compressed_size < info.size && info.size > kMaxRatio * info.compressed_size))) {
        throw BackupError("Archive has an unsafe compression ratio");
    }
}

// _index_members: each safe member name and its entry.
Members index_members(const std::vector<archive::EntryInfo>& infos, const Names& names) {
    std::unordered_set<std::string> seen;
    Members members;
    std::uint64_t total = 0;
    for (const auto& info : infos) {
        std::string name = safe_member(std::string_view(info.name), names);
        std::string folded = py::casefold(name);
        if (seen.contains(folded) || info.directory) {
            throw BackupError("Archive contains duplicate names or directories");
        }
        seen.insert(std::move(folded));
        check_member_entry(info);
        total += info.size;
        if (total > kMaxTotalBytes) {
            throw BackupError("Archive exceeds size limit");
        }
        members[std::move(name)] = &info;
    }
    return members;
}

// The bytes of a member of at most `limit` bytes; more is "Archive manifest is invalid".
std::string read_small(archive::ZipReader& zip, std::string_view name, std::uint64_t limit) {
    std::string bytes;
    zip.read_entry(name, [&](std::span<const std::byte> block) {
        if (bytes.size() + block.size() > limit) {
            throw BackupError("Archive manifest is invalid");
        }
        bytes.append(reinterpret_cast<const char*>(block.data()), block.size());
    });
    return bytes;
}

// _read_manifest.
Json read_manifest(archive::ZipReader& zip, const Members& members) {
    const auto it = members.find(std::string(kManifestName));
    if (it == members.end() || it->second->size > kMaxManifestBytes) {
        throw BackupError("Archive manifest is missing or too large");
    }
    try {
        return loads_bytes(read_small(zip, kManifestName, it->second->size));
    } catch (const py::PyError&) {
        // ValueError (UnicodeDecodeError, JSONDecodeError) and RuntimeError (RecursionError).
        throw BackupError("Archive manifest is invalid");
    } catch (const archive::BadArchive&) {
        throw BackupError("Archive manifest is invalid");
    }
}

// _check_manifest_metadata.
void check_manifest_metadata(const Json& manifest, const Names& names) {
    if (!manifest.is_object()) {
        throw BackupError("Unsupported backup format");
    }
    const Json* format = get(manifest, "format");
    const Json* version = get(manifest, "format_version");
    if (format == nullptr || !format->is_string() || *format != names.format ||
        version == nullptr || !py::py_equal(*version, Json(kFormatVersion))) {
        throw BackupError("Unsupported backup format");
    }
    const Json* created_at = get(manifest, "created_at");
    const Json* app_version = get(manifest, "app_version");
    if (created_at == nullptr || !created_at->is_string() || app_version == nullptr ||
        !app_version->is_string()) {
        throw BackupError("Archive metadata is invalid");
    }
    const Json* schema_version = get(manifest, "schema_version");
    if (schema_version == nullptr || !is_int(*schema_version) ||
        (schema_version->is_number_unsigned()
             ? schema_version->get<std::uint64_t>() >
                   static_cast<std::uint64_t>(schema::schema_version)
             : (schema_version->get<std::int64_t>() < 0 ||
                schema_version->get<std::int64_t>() > schema::schema_version))) {
        throw BackupError("Backup database schema is incompatible with this app");
    }
    const Json* include_recordings = get(manifest, "include_recordings");
    if (include_recordings == nullptr || !include_recordings->is_boolean()) {
        throw BackupError("Archive recording flag is invalid");
    }
}

bool is_sha256(const Json& digest) {
    if (!digest.is_string()) {
        return false;
    }
    const auto& text = digest.get_ref<const std::string&>();
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// _listed_file_valid.
bool listed_file_valid(const std::string& name, const Json* size, const Json* digest,
                       const std::set<std::string>& expected, const Members& members) {
    if (name == kManifestName || expected.contains(name) || !members.contains(name)) {
        return false;
    }
    if (size == nullptr || !is_int(*size) ||
        (!size->is_number_unsigned() && size->get<std::int64_t>() < 0)) {
        return false;
    }
    if (members.at(name)->size != size->get<std::uint64_t>()) {
        return false;
    }
    return digest != nullptr && is_sha256(*digest);
}

// _listed_members.
std::set<std::string> listed_members(const Json& files, const Members& members,
                                     const Names& names) {
    static const Json kNone;
    std::set<std::string> expected;
    for (const auto& item : files) {
        if (!item.is_object()) {
            throw BackupError("Archive file metadata is invalid");
        }
        const Json* path = get(item, "path");
        const std::string name = safe_member(path != nullptr ? *path : kNone, names);
        const Json* size = get(item, "size");
        const Json* digest = get(item, "sha256");
        if (!listed_file_valid(name, size, digest, expected, members)) {
            throw BackupError("Archive file metadata is invalid");
        }
        expected.insert(name);
        if (name == names.settings_member && size->get<std::uint64_t>() > kMaxManifestBytes) {
            throw BackupError("Settings file is too large");
        }
    }
    return expected;
}

std::string second_part(const std::string& name) {
    const auto first = name.find('/');
    if (first == std::string::npos) {
        throw std::out_of_range("list index out of range");
    }
    const auto second = name.find('/', first + 1);
    return name.substr(first + 1,
                       second == std::string::npos ? std::string::npos : second - first - 1);
}

} // namespace

const Names& reading_names(archive::ZipReader& zip) {
    const Names& current = names_for(identity());
    const Names& legacy = names_for(legacy_identity());
    const archive::EntryInfo* manifest = nullptr;
    for (const auto& info : zip.infolist()) {
        if (info.name == kManifestName) {
            manifest = &info;
        }
    }
    if (manifest == nullptr || manifest->size > kMaxManifestBytes) {
        return current;
    }
    try {
        const Json parsed = loads_bytes(read_small(zip, kManifestName, manifest->size));
        const Json* format = get(parsed, "format");
        if (format != nullptr && format->is_string() && *format == legacy.format) {
            return legacy;
        }
    } catch (const std::exception&) {
        // The validation proper reads it again and reports what is wrong.
    }
    return current;
}

Json validate_manifest(archive::ZipReader& zip, const Names& names) {
    const auto& infos = zip.infolist();
    if (infos.size() > kMaxFiles + 1) {
        throw BackupError("Archive has too many members");
    }
    const Members members = index_members(infos, names);
    Json manifest = read_manifest(zip, members);
    check_manifest_metadata(manifest, names);
    const Json* files = get(manifest, "files");
    if (files == nullptr || !files->is_array() || files->size() != infos.size() - 1) {
        throw BackupError("Archive file list does not match its members");
    }
    const std::set<std::string> expected = listed_members(*files, members, names);
    std::set<std::string> present;
    for (const auto& [name, info] : members) {
        if (name != kManifestName) {
            present.insert(name);
        }
    }
    if (expected != present) {
        throw BackupError("Archive contains unlisted files");
    }
    if (!expected.contains(names.database_member) || !expected.contains(names.settings_member)) {
        throw BackupError("Archive is missing core data");
    }
    if (!manifest["include_recordings"].get<bool>()) {
        for (const auto& name : expected) {
            if (is_recording_name(second_part(name))) {
                throw BackupError("Text-only archive contains recording assets");
            }
        }
    }
    const Json* warnings = get(manifest, "warnings");
    if (warnings != nullptr) {
        bool valid = warnings->is_array();
        if (valid) {
            for (const auto& warning : *warnings) {
                if (!warning.is_string() ||
                    py::len(warning.get_ref<const std::string&>()) > kMaxWarningChars) {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid) {
            throw BackupError("Archive warnings are invalid");
        }
    }
    return manifest;
}

std::optional<std::string> verify_member(archive::ZipReader& zip, const Json& item,
                                         const std::filesystem::path& db_path,
                                         const CancelToken& cancel, const Names& names) {
    const std::string& path = item.at("path").get_ref<const std::string&>();
    const std::uint64_t size = item.at("size").get<std::uint64_t>();
    Sha256 digest;
    std::uint64_t count = 0;
    std::optional<std::string> settings;
    if (path == names.settings_member) {
        settings.emplace();
    }
    std::optional<std::ofstream> output;
    if (path == names.database_member) {
        output.emplace(db_path, std::ios::binary | std::ios::trunc);
        if (!*output) {
            throw std::filesystem::filesystem_error("open", db_path,
                                                    std::make_error_code(std::errc::io_error));
        }
    }
    zip.read_entry(path, [&](std::span<const std::byte> block) {
        if (cancel.cancelled()) {
            throw BackupCancelled();
        }
        count += block.size();
        if (count > size) {
            throw BackupError("Archive member exceeds declared size");
        }
        digest.update(block);
        if (output) {
            output->write(reinterpret_cast<const char*>(block.data()),
                          static_cast<std::streamsize>(block.size()));
            if (!*output) {
                throw std::filesystem::filesystem_error("write", db_path,
                                                        std::make_error_code(std::errc::io_error));
            }
        }
        if (settings) {
            settings->append(reinterpret_cast<const char*>(block.data()), block.size());
        }
    });
    if (output) {
        output->close();
        if (output->fail()) {
            throw std::filesystem::filesystem_error("write", db_path,
                                                    std::make_error_code(std::errc::io_error));
        }
    }
    if (count != size || digest.hexdigest() != item.at("sha256").get_ref<const std::string&>()) {
        throw BackupError("Archive checksum mismatch: " + path);
    }
    return settings;
}

void check_settings_json(const std::optional<std::string>& settings_bytes) {
    if (!settings_bytes) {
        throw BackupError("Backup settings JSON is invalid");
    }
    Json settings;
    try {
        settings = loads_bytes(*settings_bytes);
    } catch (const py::ValueError&) {
        throw BackupError("Backup settings JSON is invalid");
    }
    if (!settings.is_object()) {
        throw BackupError("Backup settings JSON must be an object");
    }
}

void check_archived_meeting_paths(sql::Connection& connection, const std::set<std::string>& assets,
                                  const Names& names) {
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
            if (text == nullptr || !text->starts_with(kPathMarker)) {
                throw BackupError("Backup database has an unsafe data path");
            }
            const std::string rel = text->substr(kPathMarker.size());
            if (!rel.starts_with("meetings/")) {
                throw BackupError("Backup database has an unsafe meeting path");
            }
            safe_member("data/" + rel, names);
            if (table == schema::meeting_audio_chunks::table && !assets.contains("data/" + rel)) {
                throw BackupError("Backup database references missing meeting audio");
            }
        }
    }
}

void check_history_audio(sql::Connection& connection, const std::set<std::string>& available,
                         std::string_view unsafe_message, std::string_view missing_message,
                         const Names& names) {
    namespace th = schema::transcription_history;
    if (!has_table(connection, th::table) ||
        !columns(connection, th::table).contains(std::string(th::audio_file))) {
        return;
    }
    sql::Statement statement = connection.prepare(
        "SELECT audio_file FROM transcription_history WHERE audio_file IS NOT NULL");
    while (statement.step()) {
        const sql::Value value = statement.column(0);
        const auto* audio = std::get_if<std::string>(&value);
        if (audio == nullptr || is_absolute_text(*audio)) {
            throw BackupError(std::string(unsafe_message));
        }
        const std::string name = safe_member("data/recordings/" + *audio, names);
        if (!available.contains(name)) {
            throw BackupError(std::string(missing_message));
        }
    }
}

} // namespace fv::backup::detail
