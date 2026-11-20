#include "backup/inspect.h"

#include <optional>
#include <set>
#include <string>
#include <system_error>

#include "archive/errors.h"
#include "archive/zip_reader.h"
#include "backup/database_sanitizer.h"
#include "backup/detail/archive_check.h"
#include "backup/detail/fs.h"
#include "backup/names.h"
#include "sql/db.h"

namespace fv::backup {

namespace {

void check_cancel(const CancelToken& cancel) {
    if (cancel.cancelled()) {
        throw BackupCancelled();
    }
}

} // namespace

BackupInfo manifest_info(const std::filesystem::path& archive, const Json& manifest,
                         bool legacy_format) {
    BackupInfo info;
    info.path = archive;
    info.created_at = manifest.at("created_at").get<std::string>();
    info.app_version = manifest.at("app_version").get<std::string>();
    info.schema_version = manifest.at("schema_version").get<int>();
    info.include_recordings = manifest.at("include_recordings").get<bool>();
    const Json& files = manifest.at("files");
    info.file_count = files.size();
    for (const auto& item : files) {
        info.total_bytes += item.at("size").get<std::uint64_t>();
    }
    info.archive_bytes = detail::stat(archive).size;
    if (const auto it = manifest.find("warnings"); it != manifest.end()) {
        for (const auto& warning : *it) {
            info.warnings.push_back(warning.get<std::string>());
        }
    }
    info.legacy_format = legacy_format;
    return info;
}

BackupInfo inspect_backup(const std::filesystem::path& path, const CancelToken& cancel) {
    const std::filesystem::path archive = detail::absolute_path(path);
    if (!detail::is_file(archive)) {
        throw BackupError("Backup file does not exist");
    }
    Json manifest;
    bool legacy = false;
    try {
        const detail::TempDir work({}, std::string(identity().backup_extension) + "-inspect-");
        const std::filesystem::path db_path = work.path() / "snapshot.db";
        const Names* names = nullptr;
        {
            archive::ZipReader zip(archive);
            names = &detail::reading_names(zip);
            legacy = names->identity == &legacy_identity();
            manifest = detail::validate_manifest(zip, *names);
            std::optional<std::string> settings;
            for (const auto& item : manifest.at("files")) {
                check_cancel(cancel);
                auto output = detail::verify_member(zip, item, db_path, cancel, *names);
                if (output) {
                    settings = std::move(output);
                }
            }
            detail::check_settings_json(settings);
        }
        sql::Connection connection = sql::Connection::open(db_path, sql::OpenMode::ReadOnly);
        check_cancel(cancel);
        quick_check(connection);
        check_cancel(cancel);
        if (db_schema(connection) != manifest.at("schema_version").get<int>() ||
            meeting_is_live(connection)) {
            throw BackupError("Backup database metadata or meeting state is invalid");
        }
        std::set<std::string> assets;
        for (const auto& item : manifest.at("files")) {
            assets.insert(item.at("path").get<std::string>());
        }
        detail::check_archived_meeting_paths(connection, assets, *names);
        detail::check_history_audio(connection, assets,
                                    "Backup database has an unsafe recording path",
                                    "Backup database references missing recording audio", *names);
    } catch (const archive::BadArchive&) {
        throw BackupError("Backup ZIP is corrupt");
    } catch (const std::system_error&) {
        // OSError and EOFError; std::filesystem::filesystem_error is one.
        throw BackupError("Backup ZIP is corrupt");
    }
    return manifest_info(archive, manifest, legacy);
}

} // namespace fv::backup
