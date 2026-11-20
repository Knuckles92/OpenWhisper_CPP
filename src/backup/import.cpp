#include "backup/import.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <system_error>
#include <utility>

#include "backup/database_sanitizer.h"
#include "backup/detail/fs.h"
#include "backup/detail/staged.h"
#include "backup/import_credentials.h"
#include "backup/legacy_history.h"
#include "backup/members.h"
#include "backup/names.h"
#include "backup/restore.h"
#include "backup/sanitize.h"
#include "base/data_lease.h"
#include "base/log.h"
#include "base/utf8.h"
#include "credentials/platform_backend.h"
#include "credentials/store.h"
#include "pyport/datetime.h"
#include "settings/keys.h"
#include "sql/db.h"

namespace fv::backup {

namespace {

constexpr log::Logger logger("services.import");

// The folders of media files that neither app changes once written: hard-linked. Every other
// owned file (recorded_audio.wav, scratchpad.txt, the meeting JSON files, the recording
// journals) is rewritten in place by whichever app uses it, so a link would let one app write
// into the other's data: copied.
constexpr std::array<std::string_view, 3> kLinkedFolders = {"recordings", "meetings", "captures"};

constexpr std::string_view kParakeet = "parakeet";

void check_cancel(const CancelToken& cancel) {
    if (cancel.cancelled()) {
        throw BackupCancelled();
    }
}

void report(const Progress& progress, std::string_view phase, std::uint64_t done,
            std::uint64_t total) {
    if (progress) {
        progress(phase, done, total);
    }
}

// Path.resolve(): the stage is made under the resolved root, as the restore is.
std::filesystem::path resolve(const std::filesystem::path& path) {
    const std::filesystem::path absolute = detail::absolute_path(path);
    std::error_code error;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : resolved;
}

// datetime.now(UTC).replace(microsecond=0).isoformat().
std::string utc_now_iso() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto days = std::chrono::floor<std::chrono::days>(now);
    const std::chrono::year_month_day date(days);
    const std::chrono::hh_mm_ss time(now - days);
    py::DateTime moment;
    moment.year = static_cast<int>(date.year());
    moment.month = static_cast<int>(static_cast<unsigned>(date.month()));
    moment.day = static_cast<int>(static_cast<unsigned>(date.day()));
    moment.hour = static_cast<int>(time.hours().count());
    moment.minute = static_cast<int>(time.minutes().count());
    moment.second = static_cast<int>(time.seconds().count());
    moment.utc_offset_us = 0;
    return moment.isoformat();
}

bool regular_file(const std::filesystem::path& path) {
    return !detail::is_link(path) && detail::is_file(path);
}

// The regular files below `path` (or `path` itself), links skipped, in bytes.
std::uint64_t tree_bytes(const std::filesystem::path& path) {
    if (detail::is_link(path)) {
        return 0;
    }
    if (detail::is_file(path)) {
        return detail::stat(path).size;
    }
    if (!detail::is_dir(path)) {
        return 0;
    }
    std::uint64_t total = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(path, error);
    for (const std::filesystem::recursive_directory_iterator end; !error && it != end;
         it.increment(error)) {
        std::error_code type_error;
        if (it->is_symlink(type_error)) {
            if (it->is_directory(type_error)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (it->is_regular_file(type_error) && !type_error) {
            total += it->file_size(type_error);
        }
    }
    return total;
}

// The database file and the journal SQLite would recover from (-wal, -journal), copied into
// `folder` under the same names: SQLite opens the copy, and nothing it does (a read-only open of
// a WAL database leaves -wal and -shm files behind) reaches the source folder. nullopt when
// there is no database.
std::optional<std::filesystem::path> copy_database_out(const std::filesystem::path& database,
                                                       const std::filesystem::path& folder) {
    if (!detail::exists(database)) {
        return std::nullopt;
    }
    if (!regular_file(database)) {
        throw BackupError("Database path is unsafe");
    }
    const std::filesystem::path copy = folder / database.filename();
    std::filesystem::copy_file(database, copy);
    for (const char* suffix : {"-wal", "-journal"}) {
        std::filesystem::path sidecar = database;
        sidecar += suffix;
        if (regular_file(sidecar)) {
            std::filesystem::path target = copy;
            target += suffix;
            std::filesystem::copy_file(sidecar, target);
        }
    }
    return copy;
}

// Whether SQLite could find rows in a journal beside `database`: a -wal, or a -journal to roll
// back, that is not empty (a checkpoint with journal_size_limit leaves an empty -wal).
bool has_journal(const std::filesystem::path& database) {
    for (const char* suffix : {"-wal", "-journal"}) {
        std::filesystem::path sidecar = database;
        sidecar += suffix;
        if (detail::is_link(sidecar) || (detail::exists(sidecar) && !regular_file(sidecar)) ||
            (regular_file(sidecar) && detail::stat(sidecar).size > 0)) {
            return true;
        }
    }
    return false;
}

std::uint64_t database_rows(const std::filesystem::path& database,
                            sql::OpenMode mode = sql::OpenMode::ReadWrite) {
    sql::Connection connection = sql::Connection::open(database, mode);
    if (!has_table(connection, "transcription_history")) {
        return 0;
    }
    const sql::Value count = connection.scalar("SELECT COUNT(*) FROM transcription_history");
    const auto* value = std::get_if<std::int64_t>(&count);
    return value != nullptr && *value > 0 ? static_cast<std::uint64_t>(*value) : 0;
}

// The entries a legacy transcription_history.json would add (what merge_legacy_history reads):
// objects with a string id in its "entries" list. 0 when it cannot be read.
std::uint64_t legacy_entries(const std::filesystem::path& file) {
    try {
        if (!regular_file(file) || detail::stat(file).size > (std::uint64_t{8} << 20)) {
            return 0;
        }
        const Json data = Json::parse(detail::read_file(file));
        const auto it = data.is_object() ? data.find("entries") : data.end();
        if (it == data.end() || !it->is_array()) {
            return 0;
        }
        std::uint64_t count = 0;
        for (const auto& entry : *it) {
            const auto id = entry.is_object() ? entry.find("id") : entry.end();
            count += id != entry.end() && id->is_string() ? 1 : 0;
        }
        return count;
    } catch (const std::exception&) {
        return 0;
    }
}

void check_space(const std::filesystem::path& directory, std::uint64_t required) {
    std::filesystem::create_directories(directory);
    if (detail::free_bytes(directory) < required) {
        throw BackupError("Not enough free disk space for a safe backup or restore");
    }
}

bool linked(std::string_view relative) {
    const std::string_view top = relative.substr(0, relative.find('/'));
    return relative.size() != top.size() &&
           std::ranges::find(kLinkedFolders, top) != kLinkedFolders.end();
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (out.fail()) {
        throw std::filesystem::filesystem_error("write", path,
                                                std::make_error_code(std::errc::io_error));
    }
}

Json manifest_item(const std::string& member, const std::filesystem::path& file) {
    Json item = Json::object();
    item["path"] = member;
    item["size"] = detail::stat(file).size;
    item["sha256"] = detail::file_sha256(file);
    return item;
}

class Staging {
public:
    Staging(const AppIdentity& from, const AppIdentity& to, std::filesystem::path source,
            std::filesystem::path root, const ImportOptions& options, const Progress& progress,
            const CancelToken& cancel)
        : from_(from), from_names_(names_for(from)), names_(names_for(to)),
          source_(std::move(source)), root_(std::move(root)), options_(options),
          progress_(progress), cancel_(cancel) {}

    // Everything up to the published stage; returns the stage folder.
    std::filesystem::path run(const Json& legacy_settings) {
        check_restore_slot(root_, names_);
        const std::filesystem::path source_db =
            safe_path(source_, from_names_.database_file, from_names_);
        const auto owned = owned_files(source_, from_names_, true);
        check_cancel(cancel_);
        check_free_space(source_db, owned);

        const std::string stage_name = new_stage_name();
        home_ = restore_home(root_, names_);
        stage_ = home_ / path_from_utf8(stage_name);
        if (!std::filesystem::create_directory(stage_)) {
            throw std::filesystem::filesystem_error("mkdir", stage_,
                                                    std::make_error_code(std::errc::file_exists));
        }
        try {
            const std::filesystem::path data_stage = stage_ / "data";
            std::filesystem::create_directory(data_stage);
            const std::string at = utc_now_iso();
            const int schema = stage_database(source_db, data_stage);
            const std::filesystem::path settings_path =
                data_stage / path_from_utf8(names_.settings_file);
            write_text(settings_path,
                       detail::settings_text(import_settings(legacy_settings, path_to_utf8(source_),
                                                             at, options_.app_version)));
            Json files = Json::array();
            files.push_back(manifest_item(names_.settings_member, settings_path));
            files.push_back(manifest_item(names_.database_member,
                                          data_stage / path_from_utf8(names_.database_file)));
            stage_files(owned, data_stage, files);

            Json manifest = Json::object();
            manifest["format"] = names_.format;
            manifest["format_version"] = kFormatVersion;
            manifest["created_at"] = at;
            manifest["app_version"] = options_.app_version;
            manifest["schema_version"] = schema;
            manifest["include_recordings"] = true;
            manifest["files"] = std::move(files);
            Json warning_list = Json::array();
            for (const auto& warning : warnings_) {
                warning_list.push_back(warning);
            }
            manifest["warnings"] = std::move(warning_list);
            check_cancel(cancel_);
            publish_stage(home_, stage_name, std::move(manifest), at, names_);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(stage_, ignored);
            throw;
        }
        return stage_;
    }

    std::vector<std::string> take_warnings() { return std::move(warnings_); }

private:
    // The copies need room; links do not, unless the two folders are on different volumes.
    void check_free_space(
        const std::filesystem::path& source_db,
        const std::vector<std::pair<std::string, std::filesystem::path>>& owned) const {
        std::uint64_t database_bytes = 0;
        for (const char* suffix : {"", "-wal", "-journal"}) {
            std::filesystem::path file = source_db;
            file += suffix;
            if (regular_file(file)) {
                database_bytes += detail::stat(file).size;
            }
        }
        std::filesystem::create_directories(root_);
        const bool same_volume = detail::stat(source_).device == detail::stat(root_).device;
        // The copy SQLite reads and the staged database.
        std::uint64_t required = 2 * database_bytes + kFreeSpaceMargin;
        for (const auto& [member, path] : owned) {
            if (!same_volume || !linked(std::string_view(member).substr(5))) {
                required += detail::stat(path).size;
            }
        }
        check_space(root_, required);
    }

    // The database as a bundle carries it, then rebased onto the root as a restore rebases it.
    // Returns its schema version.
    int stage_database(const std::filesystem::path& source_db,
                       const std::filesystem::path& data_stage) {
        const std::filesystem::path staged = data_stage / path_from_utf8(names_.database_file);
        {
            const detail::TempDir copy(stage_, "snapshot-");
            const auto database = copy_database_out(source_db, copy.path());
            if (database) {
                sql::Connection connection =
                    sql::Connection::open(*database, sql::OpenMode::ReadWrite);
                if (meeting_is_live(connection)) {
                    throw BackupError(std::format(
                        "{0} has a meeting that was never ended. Open {0}, end the meeting and "
                        "close {0}, then import again.",
                        from_.product));
                }
            }
            snapshot_database(database.value_or(copy.path() / "missing.db"), staged, cancel_);
        }
        check_cancel(cancel_);
        std::vector<std::string> merge_warnings;
        try {
            merge_legacy_history(staged, source_);
        } catch (const std::exception& error) {
            // database.py _migrate_from_json logs the failure and starts without those entries,
            // so OpenWhisper's own next start would not refuse over them either.
            merge_warnings.push_back(
                std::format("Could not merge {}: {}", kLegacyHistoryFile, error.what()));
        }
        SanitizedDatabase sanitized = sanitize_database(staged, source_, true, names_);
        required_assets_ = std::move(sanitized.required_assets);
        warnings_ = std::move(merge_warnings);
        warnings_.insert(warnings_.end(), sanitized.warnings.begin(), sanitized.warnings.end());
        rebase_database(staged, root_, sanitized.schema_version, true, names_);
        return sanitized.schema_version;
    }

    void link_or_copy(const std::filesystem::path& from, const std::filesystem::path& to) const {
        try {
            if (options_.link_file) {
                options_.link_file(from, to);
            } else {
                std::filesystem::create_hard_link(from, to);
            }
            return;
        } catch (const std::exception&) {
            std::error_code ignored;
            std::filesystem::remove(to, ignored);
        }
        std::filesystem::copy_file(from, to);
    }

    void stage_files(const std::vector<std::pair<std::string, std::filesystem::path>>& owned,
                     const std::filesystem::path& data_stage, Json& files) {
        std::set<std::string> listed;
        for (const auto& [member, path] : owned) {
            listed.insert(member);
        }
        for (const auto& asset : required_assets_) {
            if (!listed.contains(asset)) {
                throw BackupError("A database-referenced recording changed during backup");
            }
        }
        std::uint64_t index = 0;
        for (const auto& [member, path] : owned) {
            check_cancel(cancel_);
            const std::string_view relative = std::string_view(member).substr(5);
            const std::filesystem::path output = safe_path(data_stage, relative, names_);
            std::filesystem::create_directories(output.parent_path());
            if (linked(relative)) {
                link_or_copy(path, output);
            } else {
                std::filesystem::copy_file(path, output);
            }
            files.push_back(manifest_item(member, output));
            report(progress_, "stage", ++index, owned.size());
        }
    }

    const AppIdentity& from_;
    const Names& from_names_;
    const Names& names_;
    const std::filesystem::path source_;
    const std::filesystem::path root_;
    const ImportOptions& options_;
    const Progress& progress_;
    const CancelToken& cancel_;
    std::filesystem::path home_;
    std::filesystem::path stage_;
    std::set<std::string> required_assets_;
    std::vector<std::string> warnings_;
};

} // namespace

std::optional<ImportSource> inspect_import_source(const std::filesystem::path& requested,
                                                  const AppIdentity& from) {
    const Names& names = names_for(from);
    const std::filesystem::path root = detail::absolute_path(requested);
    if (detail::is_link(root) || !detail::is_dir(root)) {
        return std::nullopt;
    }
    ImportSource source;
    source.root = root;
    const std::filesystem::path database = root / path_from_utf8(names.database_file);
    source.has_settings = regular_file(root / path_from_utf8(names.settings_file));
    source.has_database = regular_file(database);
    const std::filesystem::path legacy = root / path_from_utf8(kLegacyHistoryFile);
    if (!source.has_settings && !source.has_database && !regular_file(legacy)) {
        return std::nullopt;
    }
    if (source.has_database) {
        try {
            if (has_journal(database)) {
                // Rows may still be in the journal, which an immutable open never reads, and any
                // other open would create files beside it: counted from a copy.
                const detail::TempDir copy({}, "fv-import-");
                if (const auto copied = copy_database_out(database, copy.path())) {
                    source.history_entries = database_rows(*copied);
                }
            } else {
                source.history_entries = database_rows(database, sql::OpenMode::Immutable);
            }
        } catch (const std::exception& error) {
            logger.warning("Could not count the history of an import source: {}", error.what());
        }
    }
    if (source.history_entries == 0) {
        source.history_entries = legacy_entries(legacy);
    }
    for (const auto name : kRecordingNames) {
        if (name != kRemoteRecords) {
            source.recordings_bytes += tree_bytes(root / path_from_utf8(name));
        }
    }
    return source;
}

std::vector<ImportSource> find_openwhisper_data(const Paths& paths) {
    std::vector<ImportSource> sources;
    for (const auto& root : paths.openwhisper_roots()) {
        if (auto source = inspect_import_source(root)) {
            const bool repeated = std::ranges::any_of(sources, [&](const ImportSource& seen) {
                return normalize_data_root(seen.root) == normalize_data_root(source->root);
            });
            if (!repeated) {
                sources.push_back(std::move(*source));
            }
        }
    }
    return sources;
}

bool openwhisper_running(const std::filesystem::path& requested, const AppIdentity& from) {
    const std::filesystem::path root = detail::absolute_path(requested);
    // A running OpenWhisper holds its lock file; without one there is nothing to probe, and the
    // probe must not create it.
    if (!detail::is_dir(root) || !regular_file(root / path_from_utf8(from.lease_file))) {
        return false;
    }
    try {
        DataLease::acquire(root, from, LeaseMode::Exclusive).release();
        return false;
    } catch (const DataLeaseBusy&) {
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Json import_settings(const Json& legacy, std::string_view source, std::string_view at,
                     std::string_view version) {
    Json mapped = legacy.is_object() ? legacy : Json::object();
    const std::string model_key(keys::selected_model.key());
    if (const auto it = mapped.find(model_key);
        it != mapped.end() && !keys::selected_model.convert(*it)) {
        *it = std::string(kParakeet);
    }
    Json settings = sanitize_settings(mapped);
    Json record = Json::object();
    record["source"] = source;
    record["at"] = at;
    record["version"] = version;
    settings[std::string(keys::fv_import.key())] = std::move(record);
    return settings;
}

ImportResult stage_from_data_root(const AppIdentity& from, const std::filesystem::path& source,
                                  const AppIdentity& to, const std::filesystem::path& root,
                                  ImportOptions options, Progress progress, CancelToken cancel) {
    const std::filesystem::path source_root = detail::absolute_path(source);
    if (detail::is_link(source_root)) {
        throw BackupError(
            std::format("The {} data folder is a symbolic link or junction", from.product));
    }
    if (!detail::is_dir(source_root)) {
        throw BackupError(std::format("No {} data folder was found at {}", from.product,
                                      path_to_utf8(source_root)));
    }
    const std::filesystem::path data_root = resolve(root);
    if (detail::is_link(data_root)) {
        throw BackupError("The data directory is a symbolic link or junction");
    }
    if (const auto resolved_source = resolve(source_root);
        detail::inside(data_root, resolved_source) || detail::inside(resolved_source, data_root)) {
        throw BackupError(std::format("The {} data folder must be separate from the {} data folder",
                                      from.product, to.product));
    }
    check_cancel(cancel);

    DataLease lease;
    try {
        lease = DataLease::acquire(source_root, from, LeaseMode::Exclusive);
    } catch (const DataLeaseBusy&) {
        throw OpenWhisperRunning(
            std::format("{0} is running. Close {0}, then import again.", from.product));
    } catch (const StartupRestoreError& error) {
        throw BackupError(error.what());
    }
    if (pending_restore_exists(source_root, from)) {
        throw BackupError(std::format("{0} has a restore waiting for its next start. Start {0} "
                                      "once and close it, then import again.",
                                      from.product));
    }
    const Names& from_names = names_for(from);
    const std::filesystem::path settings_file =
        safe_path(source_root, from_names.settings_file, from_names);
    std::optional<std::string> settings_warning;
    Json legacy_settings;
    try {
        legacy_settings = read_settings(settings_file);
    } catch (const BackupError&) {
        if (detail::is_link(settings_file) || !detail::is_file(settings_file)) {
            throw;
        }
        // settings.py load_all_settings answers {} for a file it cannot read, so OpenWhisper
        // itself runs on its defaults: the import brings those, and the data.
        legacy_settings = Json::object();
        settings_warning = std::format(
            "{}'s settings could not be read; the default settings were imported", from.product);
    }

    ImportResult result;
    Staging staging(from, to, source_root, data_root, options, progress, cancel);
    result.stage = staging.run(legacy_settings);
    result.warnings = staging.take_warnings();
    if (settings_warning) {
        result.warnings.insert(result.warnings.begin(), *settings_warning);
    }
    lease.release();
    logger.info("Staged an import with {} warnings", result.warnings.size());

    if (options.copy_credentials) {
        try {
            std::unique_ptr<CredentialStore> own_from;
            std::unique_ptr<CredentialStore> own_to;
            CredentialStore* from_store = options.credentials_from;
            CredentialStore* to_store = options.credentials_to;
            if (from_store == nullptr) {
                own_from = std::make_unique<CredentialStore>(make_platform_backend(), from);
                from_store = own_from.get();
            }
            if (to_store == nullptr) {
                own_to = std::make_unique<CredentialStore>(make_platform_backend(), to);
                to_store = own_to.get();
            }
            const CopyReport copied =
                copy_credentials(*from_store, *to_store, legacy_settings, from, to);
            result.copied_keys = copied.copied;
            if (copied.unreadable) {
                result.warnings.push_back(
                    std::format("Could not read the keys {} saved", from.product));
            } else {
                for (const auto& name : copied.failed) {
                    result.warnings.push_back("Could not copy the saved key " + name);
                }
            }
        } catch (const std::exception&) {
            logger.warning("Could not copy the saved keys");
            result.warnings.emplace_back("Could not copy the saved keys");
        }
    }
    return result;
}

} // namespace fv::backup
