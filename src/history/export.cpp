#include "history/export.h"

#include <cstddef>
#include <format>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "base/identity.h"
#include "base/utf8.h"
#include "history/detail/export_file.h"
#include "history/detail/py_value.h"
#include "history/detail/time.h"
#include "history/detail/time_range.h"
#include "history/format_utils.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::history {

namespace {

constexpr std::size_t kFileStemMaxTitle = 60;
constexpr std::string_view kEmptyTranscript = "Empty transcript";

#ifdef _WIN32
// Python's text mode writes "\n" as the platform's line ending.
constexpr bool kTextModeCrlf = true;
#else
constexpr bool kTextModeCrlf = false;
#endif

using history_detail::field;
using history_detail::py_str;
using history_detail::text_or;
using history_detail::truthy;

// `a or b or fallback` over two keys, as text.
std::string first_text(const Json& entry, std::string_view first, std::string_view second,
                       std::string_view fallback) {
    if (const Json& value = field(entry, first); truthy(value)) {
        return py_str(value);
    }
    return text_or(entry, second, fallback);
}

std::string join(const std::vector<std::string>& lines, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += lines[i];
    }
    return out;
}

Json optional_text(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json optional_real(const std::optional<double>& value) {
    return value ? Json(*value) : Json(nullptr);
}

// format_file_size over a JSON value: numbers and bools, as Python's comparisons take them.
std::string file_size_text(const Json& value) {
    if (value.is_number() || value.is_boolean()) {
        return format_file_size(*py::py_float(value));
    }
    throw py::TypeError("'<' not supported between instances of '" +
                        std::string(value.is_string() ? "str" : value.type_name()) + "' and 'int'");
}

Json json_payload(const Json& entry) {
    Json payload = Json::object();
    for (const std::string_view key : JSON_FIELDS) {
        payload[std::string(key)] = field(entry, key);
    }
    return payload;
}

std::string json_text(const Json& value) {
    return py::dumps(value, {.indent = 2, .ensure_ascii = false});
}

std::string render_markdown(const Json& entry, const ExportOptions& options) {
    const std::string title = first_text(entry, "formatted_timestamp", "timestamp", "Untitled");
    std::vector<std::string> lines = {"## " + title, ""};
    lines.push_back("- Model: " + text_or(entry, "model", "unknown"));
    if (truthy(field(entry, "title"))) {
        lines.push_back("- Title: " + py_str(field(entry, "title")));
    }
    if (was_cleaned(entry)) {
        lines.push_back("- Cleanup: " + cleanup_label(entry));
    }
    if (const Json& source_name = field(entry, "source_name"); truthy(source_name)) {
        lines.push_back("- Source: " + py_str(source_name));
    }
    if (const Json& origin = field(entry, "origin_device_name"); truthy(origin)) {
        lines.push_back("- From: " + py_str(origin));
    }
    if (const Json& duration = field(entry, "audio_duration"); !duration.is_null()) {
        lines.push_back("- Duration: " + format_audio_duration(duration));
    }
    if (const Json& file_size = field(entry, "file_size"); !file_size.is_null()) {
        lines.push_back("- File size: " + file_size_text(file_size));
    }
    if (truthy(field(entry, "has_audio")) && truthy(field(entry, "audio_file"))) {
        lines.push_back("- Audio: " + py_str(field(entry, "audio_file")));
    }
    lines.emplace_back();
    if (options.include_cleaned) {
        const std::string text = py::strip(text_or(entry, "text", ""));
        lines.push_back(text.empty() ? std::string("_Empty transcript_") : text);
        lines.emplace_back();
    }
    if (const Json& raw = field(entry, "raw_text"); options.include_raw && truthy(raw)) {
        lines.emplace_back("### Raw");
        lines.emplace_back();
        lines.push_back(py::rstrip(py_str(raw)));
        lines.emplace_back();
    }
    return py::rstrip(join(lines, "\n")) + "\n";
}

std::string render_txt(const Json& entry) {
    const std::string stamp = first_text(entry, "formatted_timestamp", "timestamp", "");
    std::vector<std::string> lines = {"[" + stamp + "] " + text_or(entry, "model", "")};
    if (truthy(field(entry, "title"))) {
        lines.push_back("Title: " + py_str(field(entry, "title")));
    }
    if (truthy(field(entry, "source_name"))) {
        lines.push_back("Source: " + py_str(field(entry, "source_name")));
    }
    if (was_cleaned(entry)) {
        lines.push_back("Cleanup: " + cleanup_label(entry));
    }
    if (const std::string text = py::rstrip(text_or(entry, "text", "")); !text.empty()) {
        lines.push_back(text);
    }
    if (const Json& raw = field(entry, "raw_text"); truthy(raw)) {
        lines.emplace_back();
        lines.emplace_back("Raw:");
        lines.push_back(py::rstrip(py_str(raw)));
    }
    return py::rstrip(join(lines, "\n")) + "\n";
}

auto wall_key(const py::DateTime& moment) {
    return std::make_tuple(moment.year, moment.month, moment.day, moment.hour, moment.minute,
                           moment.second, moment.microsecond);
}

// Python's text mode: "\n" written as the platform's line ending.
std::string text_mode_bytes(std::string_view text) {
    if (!kTextModeCrlf) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size() + text.size() / 16);
    for (const char c : text) {
        if (c == '\n') {
            out += '\r';
        }
        out += c;
    }
    return out;
}

// open(path, "x", encoding="utf-8").write(document): false when the file exists.
bool write_new_file(const std::filesystem::path& path, std::string_view document) {
    try {
        return history_detail::create_export_file(path, text_mode_bytes(document));
    } catch (const std::filesystem::filesystem_error& error) {
        throw std::filesystem::filesystem_error("Cannot write the export file", path, error.code());
    }
}

} // namespace

std::string default_export_file_stem() {
    return py::lower(identity().product) + "_history";
}

Json serialize_history_entry(const HistoryEntry& entry, const Clock& clock) {
    Json payload = Json::object();
    payload["id"] = entry.id;
    payload["timestamp"] = entry.timestamp;
    payload["model"] = entry.model;
    payload["text"] = entry.text;
    payload["raw_text"] = optional_text(entry.raw_text);
    payload["cleanup_provider"] = optional_text(entry.cleanup_provider);
    payload["cleanup_model"] = optional_text(entry.cleanup_model);
    payload["audio_file"] = optional_text(entry.audio_file);
    payload["transcription_time"] = optional_real(entry.transcription_time);
    payload["audio_duration"] = optional_real(entry.audio_duration);
    payload["file_size"] = entry.file_size ? Json(*entry.file_size) : Json(nullptr);
    payload["source_name"] = optional_text(entry.source_name);
    payload["title"] = optional_text(entry.title);
    // Set on a host for the entries a paired computer keeps there.
    payload["origin_device_id"] = optional_text(entry.origin_device_id);
    payload["origin_device_name"] = optional_text(entry.origin_device_name);
    payload["app_id"] = optional_text(entry.app_id);
    payload["app_name"] = optional_text(entry.app_name);
    payload["app_category"] = optional_text(entry.app_category);
    payload["cleanup_level"] = optional_text(entry.cleanup_level);
    payload["entry_kind"] = optional_text(entry.entry_kind);
    payload["cleaned_text"] = optional_text(entry.cleaned_text);
    payload["language"] = optional_text(entry.language);
    payload["formatted_timestamp"] =
        entry.timestamp.empty() ? std::string() : format_timestamp(entry.timestamp, clock);
    payload["preview_text"] = preview_text(entry.text);
    payload["has_audio"] = entry.audio_file.has_value() && !entry.audio_file->empty();
    return payload;
}

std::vector<Json> list_export_entries(HistoryManager& manager, const Clock& clock) {
    return list_export_entries(manager.get_history(), clock);
}

std::vector<Json> list_export_entries(const std::vector<HistoryEntry>& history,
                                      const Clock& clock) {
    std::vector<Json> entries;
    entries.reserve(history.size());
    for (const HistoryEntry& entry : history) {
        entries.push_back(serialize_history_entry(entry, clock));
    }
    return entries;
}

std::vector<Json> filter_export_entries(const std::vector<Json>& entries,
                                        const std::optional<py::DateTime>& from_dt,
                                        const std::optional<py::DateTime>& to_dt,
                                        bool only_with_audio, const Clock& clock) {
    std::vector<Json> filtered;
    for (const Json& entry : entries) {
        if (from_dt || to_dt) {
            const auto stamped = entry_local_time(field(entry, "timestamp"), clock);
            if (!stamped) {
                continue;
            }
            if (from_dt && wall_key(*stamped) < wall_key(*from_dt)) {
                continue;
            }
            if (to_dt && wall_key(*stamped) > wall_key(*to_dt)) {
                continue;
            }
        }
        if (only_with_audio && !truthy(field(entry, "has_audio"))) {
            continue;
        }
        filtered.push_back(entry);
    }
    return filtered;
}

std::string render_entry_document(const Json& entry, std::string_view format,
                                  const ExportOptions& options) {
    if (format == FORMAT_JSON) {
        return json_text(json_payload(entry));
    }
    if (format == FORMAT_TXT) {
        return render_txt(entry);
    }
    return render_markdown(entry, options);
}

std::string render_export_document(const std::vector<Json>& entries, std::string_view format,
                                   const ExportOptions& options, const Clock& clock) {
    const py::DateTime now = history_detail::local_now(clock);
    if (format == FORMAT_JSON) {
        Json envelope = Json::object();
        envelope["format_version"] = kFormatVersion;
        envelope["exported_at"] = now.isoformat('T', "seconds");
        Json payloads = Json::array();
        for (const Json& entry : entries) {
            payloads.push_back(json_payload(entry));
        }
        envelope["entries"] = std::move(payloads);
        return json_text(envelope);
    }
    std::vector<std::string> documents;
    documents.reserve(entries.size());
    if (format == FORMAT_TXT) {
        for (const Json& entry : entries) {
            documents.push_back(render_txt(entry));
        }
        return join(documents, "\n" + std::string(60, '-') + "\n\n");
    }
    const std::string stamp = std::format("{:04}-{:02}-{:02} {:02}:{:02}", now.year, now.month,
                                          now.day, now.hour, now.minute);
    const std::string header = std::format(
        "# Transcription History Export\n\n{} transcription(s) \xC2\xB7 exported {}\n\n---\n\n",
        entries.size(), stamp);
    for (const Json& entry : entries) {
        documents.push_back(py::rstrip(render_markdown(entry, options)));
    }
    return header + join(documents, "\n\n---\n\n") + "\n";
}

std::vector<std::filesystem::path> write_per_entry_files(const std::vector<Json>& entries,
                                                         std::string_view format,
                                                         const std::filesystem::path& out_dir,
                                                         const ExportOptions& options,
                                                         const Clock& clock) {
    std::filesystem::create_directories(out_dir);
    std::unordered_set<std::string> used;
    std::vector<std::filesystem::path> written;
    for (const Json& entry : entries) {
        const std::string stem = entry_file_stem(entry, clock);
        std::string candidate = stem;
        int suffix = 2;
        const std::string document = render_entry_document(entry, format, options);
        std::string filename;
        std::filesystem::path path;
        while (true) {
            filename = candidate + "." + std::string(format);
            path = out_dir / path_from_utf8(filename);
            // Exclusive creation also closes the check-then-write race when two exports
            // target the same folder at once.
            if (used.contains(filename) || !write_new_file(path, document)) {
                candidate = std::format("{}-{}", stem, suffix);
                ++suffix;
                continue;
            }
            break;
        }
        used.insert(filename);
        written.push_back(path);
    }
    return written;
}

std::string entry_file_stem(const Json& entry, const Clock& clock) {
    static const py::Regex whitespace(R"(\s+)");
    static const py::Regex unsafe(R"([^\w\-.])");
    const auto stamped = entry_local_time(field(entry, "timestamp"), clock);
    const std::string stamp =
        stamped ? std::format("{:04}{:02}{:02}_{:02}{:02}{:02}", stamped->year, stamped->month,
                              stamped->day, stamped->hour, stamped->minute, stamped->second)
                : std::string("unknown_date");
    std::string title = py::strip(first_text(entry, "preview_text", "text", ""));
    if (title == kEmptyTranscript) {
        title.clear();
    }
    std::string slug = whitespace.sub("-", title);
    slug = py::strip(unsafe.sub("", slug), "-._");
    slug = py::rstrip(py::slice(slug, 0, static_cast<std::int64_t>(kFileStemMaxTitle)), "-._");
    return slug.empty() ? stamp : stamp + "_" + slug;
}

std::optional<py::DateTime> entry_local_time(const Json& timestamp, const Clock& clock) {
    if (!truthy(timestamp)) {
        return std::nullopt;
    }
    std::string text = py_str(timestamp);
    for (std::size_t at = text.find('Z'); at != std::string::npos; at = text.find('Z', at)) {
        text.replace(at, 1, "+00:00");
        at += 6;
    }
    py::DateTime parsed;
    try {
        parsed = py::fromisoformat(text);
    } catch (const py::ValueError&) {
        return std::nullopt;
    }
    if (!parsed.utc_offset_us) {
        return parsed;
    }
    // astimezone() with no zone: first the UTC moment, which must stay inside years 1..9999,
    // then the C runtime's localtime, which on Windows takes 1970 to 3001-01-19 only.
    const auto instant = history_detail::utc_moment(parsed);
    if (!history_detail::utc_in_range(instant)) {
        throw py::OverflowError("date value out of range");
    }
    if (!history_detail::windows_localtime_accepts(instant)) {
        throw py::PyError("OSError", "[Errno 22] Invalid argument");
    }
    const auto local = history_detail::to_zone(instant, *clock.zone());
    if (!local) {
        throw py::OverflowError("date value out of range");
    }
    return local;
}

std::string cleanup_label(const Json& entry) {
    const std::string model = text_or(entry, "cleanup_model", "");
    const std::string provider = text_or(entry, "cleanup_provider", "");
    std::string label = model.empty()
                            ? std::string("Cleaned")
                            : (provider.empty() ? model : provider + " \xC2\xB7 " + model);
    const Json& raw = field(entry, "raw_text");
    if (truthy(raw) && py::py_equal(field(entry, "text"), raw)) {
        // Undo AI edit chose the original; the AI version stays in cleaned_text.
        label += " (original kept)";
    }
    return label;
}

bool was_cleaned(const Json& entry) {
    return truthy(field(entry, "cleanup_model")) || truthy(field(entry, "raw_text"));
}

} // namespace fv::history
