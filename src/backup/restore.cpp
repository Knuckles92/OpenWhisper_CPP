#include "backup/restore.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#include "archive/zip_reader.h"
#include "backup/database_sanitizer.h"
#include "backup/detail/archive_check.h"
#include "backup/detail/fs.h"
#include "backup/detail/json_file.h"
#include "backup/detail/staged.h"
#include "backup/inspect.h"
#include "backup/members.h"
#include "backup/sanitize.h"
#include "backup/testing/restore_faults.h"
#include "base/hash.h"
#include "base/ids.h"
#include "base/utf8.h"
#include "db/schema.h"
#include "pyport/json.h"

namespace fv::backup {

namespace {

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

// _root.
std::filesystem::path resolve_root(const std::filesystem::path& requested) {
    std::filesystem::path root = detail::absolute_path(requested);
    if (detail::is_link(root)) {
        throw BackupError("The data directory is a symbolic link or junction");
    }
    return root;
}

// _check_space.
void check_space(const std::filesystem::path& directory, std::uint64_t required) {
    std::filesystem::create_directories(directory);
    if (detail::free_bytes(directory) < required) {
        throw BackupError("Not enough free disk space for a safe backup or restore");
    }
}

bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool is_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

// re.fullmatch(_STAGE_NAME_PATTERN, name).
bool is_stage_name(const Json* name) {
    if (name == nullptr || !name->is_string()) {
        return false;
    }
    const auto& text = name->get_ref<const std::string&>();
    constexpr std::string_view prefix = "stage-";
    return text.size() == prefix.size() + 32 && text.starts_with(prefix) &&
           std::all_of(text.begin() + prefix.size(), text.end(), is_lower_hex);
}

// re.fullmatch(_PREVIOUS_NAME_PATTERN, name).
bool is_previous_name(const Json* name) {
    if (name == nullptr || !name->is_string()) {
        return false;
    }
    const auto& text = name->get_ref<const std::string&>();
    constexpr std::string_view prefix = "previous-";
    return text.size() > prefix.size() && text.starts_with(prefix) &&
           std::all_of(text.begin() + prefix.size(), text.end(), is_name_char);
}

const Json* get(const Json& object, std::string_view key) {
    const auto it = object.find(std::string(key));
    return it == object.end() ? nullptr : &*it;
}

// The name a member's top-level part takes in the stage: a bundle read under another product's
// names (an .owbackup) has that product's settings and database files.
std::string staged_name(std::string_view relative, const Names& reading, const Names& names) {
    if (relative == reading.settings_file) {
        return names.settings_file;
    }
    if (relative == reading.database_file) {
        return names.database_file;
    }
    return std::string(relative);
}

// _extract_verified: every listed member streamed into the stage through its size and SHA-256.
void extract_verified(const std::filesystem::path& archive, const std::filesystem::path& data_stage,
                      const Json& manifest, const Names& reading, const Names& names,
                      const Progress& progress, const CancelToken& cancel) {
    archive::ZipReader zip(archive);
    detail::validate_manifest(zip, reading);
    const Json& files = manifest.at("files");
    std::uint64_t index = 0;
    for (const auto& item : files) {
        check_cancel(cancel);
        const std::string& member = item.at("path").get_ref<const std::string&>();
        const std::string relative =
            staged_name(std::string_view(member).substr(5), reading, names);
        const std::filesystem::path output = safe_path(data_stage, relative, names);
        std::filesystem::create_directories(output.parent_path());
        // open(output, "xb"): never over an existing file.
        if (detail::exists(output) || detail::is_link(output)) {
            throw std::filesystem::filesystem_error("open", output,
                                                    std::make_error_code(std::errc::file_exists));
        }
        const std::uint64_t size = item.at("size").get<std::uint64_t>();
        Sha256 digest;
        std::uint64_t count = 0;
        {
            std::ofstream target(output, std::ios::binary | std::ios::trunc);
            if (!target) {
                throw std::filesystem::filesystem_error("open", output,
                                                        std::make_error_code(std::errc::io_error));
            }
            zip.read_entry(member, [&](std::span<const std::byte> block) {
                check_cancel(cancel);
                count += block.size();
                if (count > size) {
                    throw BackupError("Archive member exceeds declared size");
                }
                target.write(reinterpret_cast<const char*>(block.data()),
                             static_cast<std::streamsize>(block.size()));
                if (!target) {
                    throw std::filesystem::filesystem_error(
                        "write", output, std::make_error_code(std::errc::io_error));
                }
                digest.update(block);
            });
            target.close();
            if (target.fail()) {
                throw std::filesystem::filesystem_error("write", output,
                                                        std::make_error_code(std::errc::io_error));
            }
        }
        detail::fsync_file(output);
        if (count != size ||
            digest.hexdigest() != item.at("sha256").get_ref<const std::string&>()) {
            throw BackupError("Archive checksum mismatch: " + member);
        }
        report(progress, "stage", ++index, files.size());
    }
}

// Path.write_text: the whole file replaced in place.
void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (out.fail()) {
        throw std::filesystem::filesystem_error("write", path,
                                                std::make_error_code(std::errc::io_error));
    }
}

// _move_path.
void move_path(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination.parent_path());
    if (const auto& hook = testing::restore_faults().before_move) {
        hook(source, destination);
    }
    std::filesystem::rename(source, destination);
    detail::fsync_directory(destination.parent_path());
    if (source.parent_path() != destination.parent_path()) {
        detail::fsync_directory(source.parent_path());
    }
}

// Path.unlink(missing_ok=...).
void unlink(const std::filesystem::path& path, bool missing_ok) {
    if (const auto& hook = testing::restore_faults().before_remove) {
        hook(path);
    }
    if (!std::filesystem::remove(path) && !missing_ok) {
        throw std::filesystem::filesystem_error(
            "unlink", path, std::make_error_code(std::errc::no_such_file_or_directory));
    }
}

// _load_pending.
std::pair<std::string, std::filesystem::path> load_pending(const std::filesystem::path& home) {
    const Json pending = detail::read_json(home / path_from_utf8(kPendingName));
    const Json* name = get(pending, "stage");
    if (!is_stage_name(name)) {
        throw BackupError("Pending restore metadata is unsafe");
    }
    std::string stage_name = name->get<std::string>();
    std::filesystem::path stage = home / path_from_utf8(stage_name);
    if (!detail::is_dir(stage) || detail::is_link(stage)) {
        throw BackupError("Pending restore stage is missing");
    }
    return {std::move(stage_name), std::move(stage)};
}

// _rollback: reverses any partial installation by what exists, not by journal offsets.
void rollback(const std::filesystem::path& root, const std::filesystem::path& stage,
              const std::filesystem::path& previous, const std::set<std::string>& stage_names,
              const Names& names) {
    for (auto it = names.restore_names.rbegin(); it != names.restore_names.rend(); ++it) {
        const std::string& name = *it;
        const std::filesystem::path target = safe_path(root, name, names);
        const std::filesystem::path staged = safe_path(stage / "data", name, names);
        const std::filesystem::path old = safe_path(previous, name, names);
        if (detail::exists(old)) {
            if (detail::exists(target)) {
                if (detail::exists(staged)) {
                    throw BackupError("Cannot roll back: both new copies exist");
                }
                move_path(target, staged);
            }
            move_path(old, target);
        } else if (stage_names.contains(name) && !detail::exists(staged) &&
                   detail::exists(target)) {
            // The target had no prior file and the stage item was installed.
            move_path(target, staged);
        }
    }
}

// _write_journal.
void write_journal(const std::filesystem::path& journal_path, std::string_view phase,
                   const std::string& stage_name, const std::string& previous_name,
                   const std::set<std::string>& stage_names) {
    Json journal = Json::object();
    journal["phase"] = phase;
    journal["stage"] = stage_name;
    journal["previous"] = previous_name;
    Json sorted = Json::array();
    for (const auto& name : stage_names) { // std::set orders by code point, as sorted() does
        sorted.push_back(name);
    }
    journal["stage_names"] = std::move(sorted);
    detail::write_json_atomic(journal_path, journal);
}

// _install_stage: current owned data into `previous`, staged data into place.
void install_stage(const std::filesystem::path& root, const std::filesystem::path& stage,
                   const std::filesystem::path& previous, const Names& names) {
    for (const auto& name : names.restore_names) {
        const std::filesystem::path target = safe_path(root, name, names);
        const std::filesystem::path staged = safe_path(stage / "data", name, names);
        const std::filesystem::path old = safe_path(previous, name, names);
        if (detail::exists(target)) {
            move_path(target, old);
        }
        if (detail::exists(staged)) {
            move_path(staged, target);
        }
    }
}

// _finish_restore.
void finish_restore(const std::filesystem::path& home, const std::string& previous_name,
                    const std::filesystem::path& pending_path,
                    const std::filesystem::path& journal_path) {
    Json last = Json::object();
    last["previous"] = previous_name;
    detail::write_json_atomic(home / path_from_utf8(kLastName), last);
    unlink(pending_path, true);
    unlink(journal_path, true);
}

// _resume_journal: finishes a committed restore (true) or rolls back an interrupted one (false).
bool resume_journal(const std::filesystem::path& root, const std::filesystem::path& home,
                    const std::filesystem::path& journal_path,
                    const std::filesystem::path& pending_path, const Names& names) {
    const Json journal = detail::read_json(journal_path);
    const Json* stage_name = get(journal, "stage");
    const Json* previous_name = get(journal, "previous");
    if (!is_stage_name(stage_name) || !is_previous_name(previous_name)) {
        throw BackupError("Restore journal is unsafe");
    }
    const Json* raw_names = get(journal, "stage_names");
    std::set<std::string> stage_names;
    bool valid = raw_names != nullptr && raw_names->is_array();
    if (valid) {
        for (const auto& name : *raw_names) {
            if (!name.is_string() ||
                std::ranges::find(names.restore_names, name.get_ref<const std::string&>()) ==
                    names.restore_names.end()) {
                valid = false;
                break;
            }
            stage_names.insert(name.get<std::string>());
        }
        valid = valid && stage_names.size() == raw_names->size();
    }
    if (!valid) {
        throw BackupError("Restore journal has unsafe path metadata");
    }
    const std::filesystem::path stage = home / path_from_utf8(stage_name->get<std::string>());
    const std::filesystem::path previous = home / path_from_utf8(previous_name->get<std::string>());
    const Json* phase = get(journal, "phase");
    if (phase != nullptr && *phase == "committed") {
        finish_restore(home, previous_name->get<std::string>(), pending_path, journal_path);
        return true;
    }
    if (phase == nullptr || *phase != "applying" || !detail::is_dir(stage) ||
        !detail::is_dir(previous)) {
        throw BackupError("Restore journal cannot be recovered automatically");
    }
    rollback(root, stage, previous, stage_names, names);
    unlink(journal_path, false);
    // A complete stage is available again; the caller continues with a fresh pass.
    return false;
}

// datetime.now(UTC).strftime("%Y%m%dT%H%M%SZ").
std::string utc_stamp() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%dT%H%M%SZ}", now);
}

} // namespace

std::filesystem::path restore_home(const std::filesystem::path& root, const Names& names) {
    // RESTORE_DIR is never an owned name, so this is root / RESTORE_DIR.
    return root / path_from_utf8(names.restore_dir);
}

void check_restore_slot(const std::filesystem::path& root, const Names& names) {
    const std::filesystem::path home = restore_home(root, names);
    std::filesystem::create_directories(home);
    if (detail::is_link(home) || detail::exists(home / path_from_utf8(kJournalName))) {
        throw BackupError("An earlier restore requires recovery before staging another");
    }
    if (detail::exists(home / path_from_utf8(kPendingName))) {
        throw BackupError("A restore is already waiting for restart");
    }
    const std::filesystem::path current_db = root / path_from_utf8(names.database_file);
    if (detail::is_file(current_db)) {
        sql::Connection connection =
            sql::Connection::open(current_db, sql::OpenMode::ReadOnly); // waits up to 5 s
        if (meeting_is_live(connection)) {
            throw BackupError("End the live meeting before preparing a restore");
        }
    }
}

std::string new_stage_name() {
    return "stage-" + uuid4_hex();
}

void publish_stage(const std::filesystem::path& home, std::string_view stage_name, Json manifest,
                   std::string_view created_at, const Names& names) {
    const std::filesystem::path stage = home / path_from_utf8(stage_name);
    const std::filesystem::path data_stage = stage / "data";
    const std::filesystem::path db_path = data_stage / path_from_utf8(names.database_file);
    const std::filesystem::path settings_path = data_stage / path_from_utf8(names.settings_file);
    manifest["staged_db_sha256"] = detail::file_sha256(db_path);
    manifest["staged_db_size"] = detail::stat(db_path).size;
    manifest["staged_settings_sha256"] = detail::file_sha256(settings_path);
    manifest["staged_settings_size"] = detail::stat(settings_path).size;
    detail::write_json_atomic(stage / path_from_utf8(kManifestName), manifest);
    Json pending = Json::object();
    pending["stage"] = stage_name;
    pending["created_at"] = created_at;
    detail::write_json_atomic(home / path_from_utf8(kPendingName), pending);
}

void rebase_database(const std::filesystem::path& path, const std::filesystem::path& root,
                     int expected_schema, bool include_recordings, const Names& names) {
    static constexpr std::pair<std::string_view, std::string_view> kColumns[] = {
        {schema::meeting_sessions::table, schema::meeting_sessions::spool_dir},
        {schema::meeting_audio_chunks::table, schema::meeting_audio_chunks::file_path}};
    sql::Connection connection = sql::Connection::open(path, sql::OpenMode::Create);
    quick_check(connection);
    if (db_schema(connection) != expected_schema || meeting_is_live(connection)) {
        throw BackupError("Backup database has incompatible or live meeting state");
    }
    // sqlite3 opens a transaction before the first UPDATE; leaving early rolls it back.
    sql::Transaction transaction = connection.begin();
    for (const auto& [table, column] : kColumns) {
        if (!has_table(connection, table) ||
            !columns(connection, table).contains(std::string(column))) {
            continue;
        }
        std::vector<std::pair<sql::Value, sql::Value>> rows;
        {
            sql::Statement select = connection.prepare("SELECT id, " + std::string(column) +
                                                       " FROM " + std::string(table));
            while (select.step()) {
                rows.emplace_back(select.column(0), select.column(1));
            }
        }
        sql::Statement update = connection.prepare("UPDATE " + std::string(table) + " SET " +
                                                   std::string(column) + "=? WHERE id=?");
        for (const auto& [row_id, value] : rows) {
            const auto* text = std::get_if<std::string>(&value);
            if (text == nullptr || !text->starts_with(kPathMarker)) {
                throw BackupError("Backup database contains an unsafe data path");
            }
            const std::string rel = text->substr(kPathMarker.size());
            if (!rel.starts_with("meetings/")) {
                throw BackupError("Backup database contains an unsafe meeting path");
            }
            const std::filesystem::path output = safe_path(root, rel, names);
            update.reset();
            update.bind(1, path_to_utf8(output)).bind(2, row_id);
            update.step();
        }
    }
    if (!include_recordings && has_table(connection, schema::meeting_audio_chunks::table)) {
        sql::Statement any = connection.prepare("SELECT 1 FROM meeting_audio_chunks LIMIT 1");
        if (any.step()) {
            throw BackupError("Text-only backup still contains audio chunks");
        }
    }
    transaction.commit();
    quick_check(connection);
}

std::filesystem::path prepare_restore(const std::filesystem::path& requested_archive,
                                      const std::filesystem::path& requested_root,
                                      const Progress& progress, const CancelToken& cancel) {
    const Names& names = names_for(identity());
    const std::filesystem::path root = resolve_root(requested_root);
    std::filesystem::create_directories(root);
    if (detail::is_link(root)) {
        throw BackupError("The data directory is a symbolic link");
    }
    const std::filesystem::path archive = detail::absolute_path(requested_archive);
    const BackupInfo info = inspect_backup(archive, cancel);
    check_cancel(cancel);
    check_restore_slot(root, names);
    check_space(root, info.total_bytes + kFreeSpaceMargin);
    const std::string stage_name = new_stage_name();
    const std::filesystem::path home = restore_home(root, names);
    const std::filesystem::path stage = home / path_from_utf8(stage_name);
    if (!std::filesystem::create_directory(stage)) {
        throw std::filesystem::filesystem_error("mkdir", stage,
                                                std::make_error_code(std::errc::file_exists));
    }
    try {
        Json manifest;
        const Names* reading = nullptr;
        {
            archive::ZipReader zip(archive);
            reading = &detail::reading_names(zip);
            manifest = detail::validate_manifest(zip, *reading);
        }
        const std::filesystem::path data_stage = stage / "data";
        std::filesystem::create_directory(data_stage);
        extract_verified(archive, data_stage, manifest, *reading, names, progress, cancel);
        rebase_database(data_stage / path_from_utf8(names.database_file), root, info.schema_version,
                        info.include_recordings, names);
        // A valid checksummed ZIP may have been authored by someone else: the portable-import
        // policy applies here as well as during export.
        const std::filesystem::path settings_path =
            data_stage / path_from_utf8(names.settings_file);
        const Json safe_settings = sanitize_settings(read_settings(settings_path));
        write_text(settings_path, detail::settings_text(safe_settings));
        check_cancel(cancel);
        if (reading != &names) {
            // The stage is OpenWhisper_CPP's: apply_pending_restore reads it under identity()'s
            // names.
            manifest["format"] = names.format;
            for (auto& item : manifest["files"]) {
                const std::string& member = item.at("path").get_ref<const std::string&>();
                item["path"] =
                    "data/" + staged_name(std::string_view(member).substr(5), *reading, names);
            }
        }
        publish_stage(home, stage_name, std::move(manifest), info.created_at, names);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(stage, ignored);
        throw;
    }
    return stage;
}

bool pending_restore_exists(const std::filesystem::path& requested_root, const AppIdentity& app) {
    const Names& names = names_for(app);
    const std::filesystem::path home = restore_home(resolve_root(requested_root), names);
    if (detail::is_link(home)) {
        throw BackupError("Restore directory is a symbolic link");
    }
    return detail::exists(home / path_from_utf8(kPendingName)) ||
           detail::exists(home / path_from_utf8(kJournalName));
}

std::optional<std::filesystem::path> previous_data_dir(const std::filesystem::path& requested_root,
                                                       const AppIdentity& app) {
    const Names& names = names_for(app);
    const std::filesystem::path home = restore_home(resolve_root(requested_root), names);
    const std::filesystem::path path = home / path_from_utf8(kLastName);
    if (!detail::is_file(path)) {
        return std::nullopt;
    }
    const Json data = detail::read_json(path);
    const Json* name = get(data, "previous");
    if (!is_previous_name(name)) {
        throw BackupError("Restore history metadata is invalid");
    }
    return home / path_from_utf8(name->get<std::string>());
}

bool apply_pending_restore(const std::filesystem::path& requested_root, const AppIdentity& app) {
    const Names& names = names_for(app);
    const std::filesystem::path root = resolve_root(requested_root);
    const std::filesystem::path home = restore_home(root, names);
    const std::filesystem::path journal_path = home / path_from_utf8(kJournalName);
    const std::filesystem::path pending_path = home / path_from_utf8(kPendingName);
    if (!detail::exists(pending_path) && !detail::exists(journal_path)) {
        return false;
    }
    if (detail::is_link(home)) {
        throw BackupError("Restore directory is a symbolic link");
    }
    if (detail::exists(journal_path) &&
        resume_journal(root, home, journal_path, pending_path, names)) {
        return true;
    }
    if (!detail::exists(pending_path)) {
        return false;
    }
    const auto [stage_name, stage] = load_pending(home);
    detail::restore_stage_check(stage, root, names);
    const std::string previous_name = "previous-" + utc_stamp() + "-" + uuid4_hex().substr(0, 8);
    const std::filesystem::path previous = home / path_from_utf8(previous_name);
    if (!std::filesystem::create_directory(previous)) {
        throw std::filesystem::filesystem_error("mkdir", previous,
                                                std::make_error_code(std::errc::file_exists));
    }
    detail::fsync_directory(home);
    std::set<std::string> stage_names;
    for (const auto& name : names.restore_names) {
        if (detail::exists(stage / "data" / path_from_utf8(name))) {
            stage_names.insert(name);
        }
    }
    write_journal(journal_path, "applying", stage_name, previous_name, stage_names);
    try {
        install_stage(root, stage, previous, names);
        write_journal(journal_path, "committed", stage_name, previous_name, stage_names);
    } catch (const testing::SimulatedCrash&) {
        throw; // a dead process rolls nothing back: the journal recovers it at the next start
    } catch (...) {
        rollback(root, stage, previous, stage_names, names);
        unlink(journal_path, true);
        throw;
    }
    finish_restore(home, previous_name, pending_path, journal_path);
    std::error_code ignored;
    std::filesystem::remove_all(stage, ignored);
    return true;
}

} // namespace fv::backup
