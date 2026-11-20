#include "backup/create.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "archive/zip_writer.h"
#include "backup/database_sanitizer.h"
#include "backup/detail/fs.h"
#include "backup/inspect.h"
#include "backup/legacy_history.h"
#include "backup/members.h"
#include "backup/names.h"
#include "backup/sanitize.h"
#include "base/atomic_file.h"
#include "base/env.h"
#include "base/hash.h"
#include "base/paths.h"
#include "base/utf8.h"
#include "pyport/datetime.h"
#include "pyport/json.h"
#include "pyport/text.h"

namespace fv::backup {

namespace {

// Suffixes of audio, which deflate cannot shrink: stored as they are.
constexpr std::array<std::string_view, 6> kStoredSuffixes = {".wav", ".mp3", ".flac",
                                                             ".ogg", ".m4a", ".mp4"};

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
std::filesystem::path data_root(const std::filesystem::path& requested) {
    const std::filesystem::path root = requested.empty()
                                           ? Paths::resolve(identity(), system_env()).data_root()
                                           : detail::absolute_path(requested);
    if (detail::is_link(root)) {
        throw BackupError("The data directory is a symbolic link or junction");
    }
    return root;
}

// _source_signature: (st_dev, st_ino, st_size, st_mtime_ns) of a regular source file.
detail::FileStat source_signature(const std::filesystem::path& path) {
    detail::FileStat details;
    try {
        details = detail::stat(path);
    } catch (const std::filesystem::filesystem_error&) {
        throw BackupError("Source data became unavailable during backup: " + detail::name_of(path));
    }
    if (detail::is_link(path) || !details.regular) {
        throw BackupError("A source file became a symbolic link or special file");
    }
    return details;
}

struct Record {
    Json item;
    detail::FileStat signature;
};

// _archive_file.
Record archive_file(archive::ZipWriter& zip, const std::string& name,
                    const std::filesystem::path& source, const CancelToken& cancel,
                    const Names& names) {
    safe_member(name, names);
    const detail::FileStat signature = source_signature(source);
    if (signature.size > kMaxFileBytes) {
        throw BackupError("A source file is not regular or exceeds the size limit");
    }
    const std::string suffix = py::lower(detail::suffix(source));
    const archive::Method method =
        std::ranges::find(kStoredSuffixes, suffix) != kStoredSuffixes.end()
            ? archive::Method::Stored
            : archive::Method::Deflated;
    detail::InputFile input(source);
    const detail::FileStat opened = input.stat();
    if (opened.device != signature.device || opened.inode != signature.inode ||
        opened.size != signature.size) {
        throw BackupError("Data changed during backup: " + name);
    }
    Sha256 digest;
    std::uint64_t count = 0;
    zip.add_stream(name, method, kRegularFileAttr, [&](std::span<std::byte> buffer) {
        const std::size_t got = input.read(buffer.first(std::min(buffer.size(), kChunkSize)));
        if (got == 0) {
            return got;
        }
        check_cancel(cancel);
        digest.update(buffer.first(got));
        count += got;
        return got;
    });
    if (source_signature(source) != signature || count != signature.size) {
        throw BackupError("Data changed during backup: " + name);
    }
    Json item = Json::object();
    item["path"] = name;
    item["size"] = count;
    item["sha256"] = digest.hexdigest();
    return {std::move(item), signature};
}

void check_destination(const std::filesystem::path& destination, const std::filesystem::path& root,
                       const Names& names) {
    if (py::lower(detail::suffix(destination)) != names.extension) {
        throw BackupError("Backup file must end in " + names.extension);
    }
    for (const auto recording : kRecordingNames) {
        if (detail::inside(destination, root / path_from_utf8(recording))) {
            throw BackupError("Choose a backup destination outside recorded data");
        }
    }
    if (detail::inside(destination, root / path_from_utf8(names.restore_dir))) {
        throw BackupError("Choose a backup destination outside recorded data");
    }
}

struct SourceSettings {
    std::optional<detail::FileStat> signature;
    Json settings;
};

// _read_source_settings: the live settings and the file signature they were read under.
SourceSettings read_source_settings(const std::filesystem::path& path) {
    SourceSettings result;
    if (detail::exists(path)) {
        result.signature = source_signature(path);
    }
    result.settings = read_settings(path);
    if (result.signature && source_signature(path) != *result.signature) {
        throw BackupError("Settings changed during backup");
    }
    return result;
}

void check_sources_unchanged(
    const std::vector<std::pair<std::filesystem::path, detail::FileStat>>& sources,
    const std::filesystem::path& settings_path,
    const std::optional<detail::FileStat>& settings_signature) {
    for (const auto& [path, signature] : sources) {
        if (source_signature(path) != signature) {
            throw BackupError("Data changed during backup: " + detail::name_of(path));
        }
    }
    const bool changed = settings_signature ? source_signature(settings_path) != *settings_signature
                                            : detail::exists(settings_path);
    if (changed) {
        throw BackupError("Settings changed during backup");
    }
}

// _check_space.
void check_space(const std::filesystem::path& directory, std::uint64_t required) {
    std::filesystem::create_directories(directory);
    if (detail::free_bytes(directory) < required) {
        throw BackupError("Not enough free disk space for a safe backup or restore");
    }
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

// json.dumps(settings, ensure_ascii=False, indent=2) + "\n", written in text mode: CRLF line
// ends on Windows, as Path.write_text writes them.
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

} // namespace

BackupInfo create_backup(const std::filesystem::path& requested, const CreateOptions& options) {
    const Names& names = names_for(identity());
    const CancelToken& cancel = options.cancel;
    const std::filesystem::path root = data_root(options.data_root);
    const std::filesystem::path destination = detail::absolute_path(requested);
    check_destination(destination, root, names);
    std::filesystem::create_directories(destination.parent_path());
    check_cancel(cancel);
    Json manifest;
    {
        const detail::TempDir work(destination.parent_path(), names.extension + "-build-");
        const std::filesystem::path db_copy = work.path() / path_from_utf8(names.database_file);
        snapshot_database(safe_path(root, names.database_file, names), db_copy, cancel);
        merge_legacy_history(db_copy, root);
        SanitizedDatabase database =
            sanitize_database(db_copy, root, options.include_recordings, names);
        auto owned = owned_files(root, names, options.include_recordings);
        std::set<std::string> listed;
        for (const auto& [name, path] : owned) {
            listed.insert(name);
        }
        for (const auto& asset : database.required_assets) {
            if (!listed.contains(asset)) {
                throw BackupError("A database-referenced recording changed during backup");
            }
        }
        if (owned.size() + 2 > kMaxFiles) {
            throw BackupError("Too many files to archive");
        }
        report(options.progress, "scan", owned.size(), owned.size());
        const std::filesystem::path settings_path = safe_path(root, names.settings_file, names);
        const SourceSettings source = read_source_settings(settings_path);
        const Json settings = sanitize_settings(source.settings);
        std::vector<std::string> warnings = std::move(database.warnings);
        if (const auto it = source.settings.find("remote_records_location");
            it != source.settings.end() && (*it == "host" || *it == "both")) {
            warnings.emplace_back("Host-kept-only records are not in this local archive");
        }
        const std::filesystem::path settings_copy =
            work.path() / path_from_utf8(names.settings_file);
        write_file_atomic(settings_copy, settings_text(settings), {.fsync = false});

        std::vector<std::pair<std::string, std::filesystem::path>> files;
        files.emplace_back(names.settings_member, settings_copy);
        files.emplace_back(names.database_member, db_copy);
        files.insert(files.end(), owned.begin(), owned.end());
        std::uint64_t total = 0;
        for (const auto& [name, path] : files) {
            total += detail::stat(path).size;
        }
        if (total > kMaxTotalBytes) {
            throw BackupError("Data exceeds backup size limit");
        }
        check_space(destination.parent_path(), total + kFreeSpaceMargin);

        const std::filesystem::path temp_archive =
            work.path() / path_from_utf8("bundle" + names.extension);
        Json records = Json::array();
        std::vector<std::pair<std::filesystem::path, detail::FileStat>> sources;
        {
            archive::ZipWriter zip = archive::ZipWriter::create(temp_archive);
            std::uint64_t index = 0;
            for (const auto& [name, path] : files) {
                check_cancel(cancel);
                const bool from_work = detail::inside(path, work.path());
                if (!from_work) {
                    safe_path(root, std::string_view(name).substr(5), names);
                }
                Record record = archive_file(zip, name, path, cancel, names);
                records.push_back(std::move(record.item));
                if (!from_work) {
                    sources.emplace_back(path, record.signature);
                }
                report(options.progress, "archive", ++index, files.size());
            }
            manifest = Json::object();
            manifest["format"] = names.format;
            manifest["format_version"] = kFormatVersion;
            manifest["created_at"] = utc_now_iso();
            manifest["app_version"] = options.app_version;
            manifest["schema_version"] = database.schema_version;
            manifest["include_recordings"] = options.include_recordings;
            manifest["files"] = std::move(records);
            Json warning_list = Json::array();
            for (auto& warning : warnings) {
                warning_list.push_back(std::move(warning));
            }
            manifest["warnings"] = std::move(warning_list);
            py::DumpOptions compact;
            compact.ensure_ascii = false;
            compact.item_sep = ",";
            compact.key_sep = ":";
            const std::string text = py::dumps(manifest, compact);
            zip.add_bytes(kManifestName, std::as_bytes(std::span(text)), archive::Method::Deflated,
                          kRegularFileAttr);
            zip.close();
        }
        check_cancel(cancel);
        inspect_backup(temp_archive, cancel); // the exact bytes, before publication
        check_cancel(cancel);
        check_sources_unchanged(sources, settings_path, source.signature);
        detail::fsync_file(temp_archive);
        std::filesystem::rename(temp_archive, destination);
        detail::fsync_directory(destination.parent_path());
    }
    return manifest_info(destination, manifest, false);
}

} // namespace fv::backup
