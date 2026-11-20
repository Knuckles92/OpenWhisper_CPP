#include "backup/preferences.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <system_error>

#include "backup/detail/fs.h"
#include "base/atomic_file.h"
#include "base/utf8.h"
#include "pyport/json.h"
#include "pyport/regex.h"

namespace fv::backup {

namespace {

constexpr double kLoadSlackSeconds = 86400; // last_auto_at may be up to a day ahead of now

#ifdef _WIN32
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif

// A JSON number (or bool) as Python's arithmetic reads it; nullopt for anything else.
std::optional<double> number_of(const Json& value) {
    if (value.is_boolean()) {
        return value.get<bool>() ? 1.0 : 0.0;
    }
    if (value.is_number()) {
        return value.get<double>();
    }
    return std::nullopt;
}

bool is_symbolic_link(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
}

// Path.resolve(): absolute, links resolved as far as the path exists.
std::filesystem::path resolve(const std::filesystem::path& path) {
    const std::filesystem::path absolute = detail::absolute_path(path);
    std::error_code error;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(absolute, error);
    return error ? absolute.lexically_normal() : resolved;
}

// Path equality: parts compared case-insensitively on Windows.
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    return detail::relative_parts(a, b) ==
               std::optional<std::vector<std::string>>(std::vector<std::string>{}) &&
           detail::relative_parts(b, a) ==
               std::optional<std::vector<std::string>>(std::vector<std::string>{});
}

// str(Path(text)): separators normalized, "." and empty parts dropped, no trailing separator.
std::string pathlib_str(std::string_view text) {
    const std::filesystem::path path = path_from_utf8(text);
    std::string anchor = path_to_utf8(path.root_name()) + path_to_utf8(path.root_directory());
    const char separator = kWindows ? '\\' : '/';
    if constexpr (kWindows) {
        std::ranges::replace(anchor, '/', '\\');
    }
    std::string joined;
    for (const auto& element : path.relative_path()) {
        const std::string part = path_to_utf8(element);
        if (part.empty() || part == ".") {
            continue;
        }
        if (!joined.empty()) {
            joined += separator;
        }
        joined += part;
    }
    std::string result = anchor + joined;
    return result.empty() ? "." : result;
}

// re.escape.
std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80 && !std::isalnum(byte) && c != '_') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace

std::optional<std::int64_t> schedule_interval(std::string_view frequency) {
    if (frequency == "daily") {
        return 86400;
    }
    if (frequency == "weekly") {
        return 7 * 86400;
    }
    return std::nullopt;
}

Json BackupPreferences::to_json() const {
    Json values = Json::object();
    values["frequency"] = frequency;
    values["retention_count"] = retention_count;
    values["destination"] = destination;
    values["last_backup"] = last_backup;
    values["last_auto_at"] = last_auto_at;
    Json archives = Json::array();
    for (const auto& path : scheduled_archives) {
        archives.push_back(path);
    }
    values["scheduled_archives"] = std::move(archives);
    return values;
}

BackupPreferences BackupPreferences::from_json(const Json& values) {
    BackupPreferences preferences;
    if (!values.is_object()) {
        return preferences;
    }
    if (const auto it = values.find("frequency"); it != values.end() && it->is_string()) {
        preferences.frequency = it->get<std::string>();
    }
    if (const auto it = values.find("retention_count");
        it != values.end() && it->is_number_integer()) {
        preferences.retention_count = it->get<int>();
    }
    if (const auto it = values.find("destination"); it != values.end() && it->is_string()) {
        preferences.destination = it->get<std::string>();
    }
    if (const auto it = values.find("last_backup"); it != values.end()) {
        preferences.last_backup = *it;
    }
    if (const auto it = values.find("last_auto_at"); it != values.end()) {
        preferences.last_auto_at = *it;
    }
    if (const auto it = values.find("scheduled_archives"); it != values.end() && it->is_array()) {
        for (const auto& path : *it) {
            if (path.is_string()) {
                preferences.scheduled_archives.push_back(path.get<std::string>());
            }
        }
    }
    return preferences;
}

double BackupPreferences::last_auto_seconds() const {
    return number_of(last_auto_at).value_or(0.0);
}

BackupPreferences load_preferences(const std::filesystem::path& data_dir, double now) {
    BackupPreferences preferences;
    const std::filesystem::path source = data_dir / path_from_utf8(kPreferencesFile);
    Json value;
    try {
        if (is_symbolic_link(source) || detail::stat(source).size > kMaxPreferencesBytes) {
            return preferences;
        }
        const std::string text = detail::read_file(source);
        if (!detail::is_valid_utf8(text)) {
            return preferences; // UnicodeDecodeError
        }
        value = py::loads(text);
    } catch (const std::filesystem::filesystem_error&) {
        return preferences;
    } catch (const py::ValueError&) {
        return preferences;
    }
    if (!value.is_object()) {
        return preferences;
    }
    if (const auto it = value.find("frequency");
        it != value.end() && it->is_string() &&
        (*it == "off" || *it == "daily" || *it == "weekly")) {
        preferences.frequency = it->get<std::string>();
    }
    if (const auto it = value.find("retention_count");
        it != value.end() && it->is_number_integer()) {
        const bool in_range = it->is_number_unsigned()
                                  ? it->get<std::uint64_t>() >= 1 && it->get<std::uint64_t>() <= 100
                                  : it->get<std::int64_t>() >= 1 && it->get<std::int64_t>() <= 100;
        if (in_range) {
            preferences.retention_count = it->get<int>();
        }
    }
    if (const auto it = value.find("destination"); it != value.end() && it->is_string()) {
        preferences.destination = it->get<std::string>();
    }
    if (const auto it = value.find("last_backup"); it != value.end() && it->is_object()) {
        if (const auto path = it->find("path"); path != it->end() && path->is_string()) {
            preferences.last_backup = *it;
        }
    }
    const auto stamp = value.find("last_auto_at");
    const Json stamp_value = stamp == value.end() ? Json(0) : *stamp;
    if (const auto seconds = number_of(stamp_value);
        seconds && *seconds >= 0 && *seconds <= now + kLoadSlackSeconds) {
        preferences.last_auto_at = stamp_value;
    }
    if (const auto it = value.find("scheduled_archives"); it != value.end() && it->is_array()) {
        const std::size_t first =
            it->size() > kMaxScheduledArchives ? it->size() - kMaxScheduledArchives : 0;
        for (std::size_t i = first; i < it->size(); ++i) {
            if ((*it)[i].is_string()) {
                preferences.scheduled_archives.push_back((*it)[i].get<std::string>());
            }
        }
    }
    return preferences;
}

void save_preferences(const std::filesystem::path& data_dir, const BackupPreferences& preferences) {
    std::filesystem::create_directories(data_dir);
    const std::filesystem::path destination = data_dir / path_from_utf8(kPreferencesFile);
    if (is_symbolic_link(destination)) {
        throw BackupError("The backup preferences file must not be a symbolic link.");
    }
    py::DumpOptions options;
    options.indent = 2;
    std::string text = py::dumps(preferences.to_json(), options);
    if constexpr (kWindows) {
        std::string crlf;
        crlf.reserve(text.size() + text.size() / 8);
        for (const char c : text) {
            if (c == '\n') {
                crlf += '\r';
            }
            crlf += c;
        }
        text = std::move(crlf);
    }
    write_file_atomic(destination, text);
}

std::optional<double> next_backup_at(const BackupPreferences& preferences, double now,
                                     double retry_after) {
    const auto interval = schedule_interval(preferences.frequency);
    if (!interval) {
        return std::nullopt;
    }
    const double last = preferences.last_auto_seconds();
    const double due = last != 0 ? last + static_cast<double>(*interval) : 0.0;
    return std::max({now, due, retry_after});
}

Json backup_info_json(const BackupInfo& info) {
    Json values = Json::object();
    values["path"] = path_to_utf8(info.path);
    values["created_at"] = info.created_at;
    values["app_version"] = info.app_version;
    values["schema_version"] = info.schema_version;
    values["include_recordings"] = info.include_recordings;
    values["file_count"] = info.file_count;
    values["total_bytes"] = info.total_bytes;
    values["archive_bytes"] = info.archive_bytes;
    Json warnings = Json::array();
    for (const auto& warning : info.warnings) {
        warnings.push_back(warning);
    }
    values["warnings"] = std::move(warnings);
    return values;
}

std::string auto_backup_name(const AppIdentity& app, std::chrono::system_clock::time_point at,
                             std::string_view hex12) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(at);
    return std::format("{}-auto-{:%Y%m%dT%H%M%SZ}-{}{}", app.product, seconds, hex12,
                       app.backup_extension);
}

bool is_auto_backup_name(std::string_view name, const AppIdentity& app) {
    const py::Regex pattern(escape(app.product) + R"(-auto-\d{8}T\d{6}Z-[0-9a-f]{12})" +
                            escape(app.backup_extension) + R"(\Z)");
    return pattern.fullmatch(name).has_value();
}

PruneSelection prune_selection(const BackupPreferences& preferences, const AppIdentity& app) {
    const std::filesystem::path folder = resolve(path_from_utf8(preferences.destination));
    std::vector<std::filesystem::path> owned;
    for (const auto& value : preferences.scheduled_archives) {
        // Path(value): its parent and name as pathlib splits the normalized text.
        const std::filesystem::path candidate = path_from_utf8(pathlib_str(value));
        std::filesystem::path parent = candidate.parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        if (same_path(resolve(parent), folder) &&
            is_auto_backup_name(detail::name_of(candidate), app) && !is_symbolic_link(candidate) &&
            detail::is_file(candidate)) {
            owned.push_back(candidate);
        }
    }
    // owned[:-keep]
    const auto count = static_cast<std::ptrdiff_t>(owned.size());
    const std::ptrdiff_t keep = preferences.retention_count;
    const std::ptrdiff_t end =
        keep > 0 ? std::max<std::ptrdiff_t>(0, count - keep) : std::min(count, -keep);
    PruneSelection selection;
    selection.remove.assign(owned.begin(), owned.begin() + end);
    std::vector<std::string> removed;
    for (const auto& path : selection.remove) {
        removed.push_back(pathlib_str(path_to_utf8(path)));
    }
    for (const auto& value : preferences.scheduled_archives) {
        if (std::ranges::find(removed, value) == removed.end()) {
            selection.remaining.push_back(value);
        }
    }
    return selection;
}

void prune_automatic_backups(BackupPreferences& preferences, const AppIdentity& app) {
    PruneSelection selection = prune_selection(preferences, app);
    for (const auto& candidate : selection.remove) {
        // Only exact files recorded after successful automatic creation are eligible; manual
        // archives and unrelated destination files remain.
        if (!std::filesystem::remove(candidate)) {
            throw std::filesystem::filesystem_error(
                "unlink", candidate, std::make_error_code(std::errc::no_such_file_or_directory));
        }
    }
    preferences.scheduled_archives = std::move(selection.remaining);
}

} // namespace fv::backup
