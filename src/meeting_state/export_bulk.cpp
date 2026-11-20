#include "meeting_state/export_bulk.h"

#include <format>
#include <fstream>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "base/utf8.h"
#include "meeting_state/content.h"
#include "meeting_state/detail/extras.h"
#include "meeting_state/detail/py_value.h"
#include "meeting_state/detail/time.h"
#include "meeting_state/export_json.h"
#include "meeting_state/export_markdown.h"
#include "meeting_state/export_txt.h"
#include "meeting_state/stored.h"
#include "meeting_state/time_utils.h"
#include "pyport/json.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

using detail::get;
using detail::truthy;

constexpr std::size_t kFileStemMaxTitle = 60;

#ifdef _WIN32
// Python's text mode writes "\n" as the platform's line ending.
constexpr bool kTextModeCrlf = true;
#else
constexpr bool kTextModeCrlf = false;
#endif

bool live(const Json& meeting) {
    const Json& status = get(meeting, "status");
    const std::string lowered = py::lower(truthy(status) ? detail::py_str(status) : "");
    return lowered == "active" || lowered == "paused" || lowered == "ending";
}

auto wall_key(const py::DateTime& moment) {
    return std::tuple(moment.year, moment.month, moment.day, moment.hour, moment.minute,
                      moment.second, moment.microsecond);
}

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

// open(path, "x", encoding="utf-8").write(document): false, writing nothing, when the file
// exists. Exclusive creation also closes the race of two exports into one folder.
bool write_new_file(const std::filesystem::path& path, std::string_view document) {
    std::ofstream file(path, std::ios::out | std::ios::binary | std::ios::noreplace);
    if (!file.is_open()) {
        std::error_code ignored;
        if (std::filesystem::exists(path, ignored)) {
            return false;
        }
        throw std::filesystem::filesystem_error("Cannot write the export file", path,
                                                std::make_error_code(std::errc::permission_denied));
    }
    const std::string bytes = text_mode_bytes(document);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) {
        throw std::filesystem::filesystem_error("Cannot write the export file", path,
                                                std::make_error_code(std::errc::io_error));
    }
    return true;
}

} // namespace

std::vector<Json> list_export_meetings(MeetingRepository& repository) {
    std::vector<Json> meetings;
    for (Json meeting : repository.list_meetings()) {
        if (live(meeting)) {
            continue;
        }
        if (!meeting.contains("content_summary")) {
            const Json& id = get(meeting, "id");
            meeting["content_summary"] =
                summarize_meeting_content(repository, truthy(id) ? detail::py_str(id) : "");
        }
        meetings.push_back(std::move(meeting));
    }
    return meetings;
}

std::vector<Json> filter_export_meetings(const std::vector<Json>& meetings,
                                         const std::optional<py::DateTime>& from_dt,
                                         const std::optional<py::DateTime>& to_dt,
                                         bool only_with_transcript, const Clock& clock) {
    std::vector<Json> filtered;
    for (const Json& meeting : meetings) {
        if (live(meeting)) {
            continue;
        }
        if (from_dt || to_dt) {
            const auto started = as_local_time(get(meeting, "started_at"), clock);
            if (!started) {
                continue;
            }
            if (from_dt && wall_key(*started) < wall_key(*from_dt)) {
                continue;
            }
            if (to_dt && wall_key(*started) > wall_key(*to_dt)) {
                continue;
            }
        }
        if (only_with_transcript) {
            const Json& summary = get(meeting, "content_summary");
            const Json& has_transcript = get(summary, "has_transcript");
            if (summary.is_object() && has_transcript.is_boolean() && !has_transcript.get<bool>()) {
                continue;
            }
        }
        filtered.push_back(meeting);
    }
    return filtered;
}

std::optional<MeetingExport> collect_meeting_export(MeetingRepository& repository,
                                                    std::string_view meeting_id) {
    auto meeting = repository.get_meeting(meeting_id);
    if (!meeting) {
        return std::nullopt;
    }
    MeetingExport entry;
    entry.state = stored_state_dict(*meeting);
    entry.segments = repository.get_segments(meeting_id, -1.0, std::nullopt);
    entry.meeting = std::move(*meeting);
    return entry;
}

std::string render_meeting_document(const MeetingExport& entry, std::string_view format,
                                    const BulkExportOptions& options, const Clock& clock) {
    if (format == FORMAT_JSON) {
        return export_json(entry.meeting, entry.state, entry.segments);
    }
    if (format == FORMAT_TXT) {
        return export_transcript_txt(entry.meeting, entry.state, entry.segments, clock);
    }
    return export_markdown(entry.meeting, entry.state, entry.segments, options.include_transcript,
                           options.include_intelligence, clock);
}

std::string render_combined(const std::vector<MeetingExport>& entries, std::string_view format,
                            const BulkExportOptions& options, const Clock& clock) {
    const py::DateTime now = detail::local_now(clock);
    if (format == FORMAT_JSON) {
        Json meetings = Json::array();
        for (const MeetingExport& entry : entries) {
            meetings.push_back(export_json_payload(entry.meeting, entry.state, entry.segments));
        }
        Json envelope = Json::object();
        envelope["format_version"] = EXPORT_FORMAT_VERSION;
        envelope["exported_at"] = now.isoformat('T', "seconds");
        envelope["meetings"] = std::move(meetings);
        py::DumpOptions dump;
        dump.indent = 2;
        dump.ensure_ascii = false;
        return py::dumps(envelope, dump);
    }
    std::vector<std::string> documents;
    documents.reserve(entries.size());
    if (format == FORMAT_TXT) {
        for (const MeetingExport& entry : entries) {
            documents.push_back(render_meeting_document(entry, format, options, clock));
        }
        return detail::join(documents, "\n\n" + std::string(60, '=') + "\n\n");
    }
    const std::string stamp = std::format("{:04}-{:02}-{:02} {:02}:{:02}", now.year, now.month,
                                          now.day, now.hour, now.minute);
    const std::string header =
        std::format("# Past Meetings Export\n\n{} meeting(s) \xC2\xB7 exported {}\n\n---\n\n",
                    entries.size(), stamp);
    for (const MeetingExport& entry : entries) {
        documents.push_back(py::rstrip(render_meeting_document(entry, format, options, clock)));
    }
    return header + detail::join(documents, "\n\n---\n\n") + "\n";
}

std::vector<std::filesystem::path>
write_per_meeting_files(const std::filesystem::path& dir, std::string_view format,
                        const std::vector<MeetingExport>& entries, const BulkExportOptions& options,
                        const Clock& clock) {
    std::filesystem::create_directories(dir);
    std::unordered_set<std::string> used;
    std::vector<std::filesystem::path> written;
    for (const MeetingExport& entry : entries) {
        // Names collide only for same-second untitled meetings; a numeric suffix tells them
        // apart within the chosen folder.
        const std::string stem = meeting_file_stem(entry.meeting, entry.state, clock);
        std::string candidate = stem;
        int suffix = 2;
        const std::string document = render_meeting_document(entry, format, options, clock);
        std::string filename;
        std::filesystem::path path;
        while (true) {
            filename = candidate + "." + std::string(format);
            path = dir / path_from_utf8(filename);
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

std::string meeting_file_stem(const Json& meeting, const Json& state, const Clock& clock) {
    static const py::Regex whitespace(R"(\s+)");
    static const py::Regex unsafe(R"([^\w\-.])");
    const auto started = as_local_time(get(meeting, "started_at"), clock);
    const std::string stamp =
        started ? std::format("{:04}{:02}{:02}_{:02}{:02}{:02}", started->year, started->month,
                              started->day, started->hour, started->minute, started->second)
                : std::string("unknown_date");
    const std::string title = py::strip(resolve_title(meeting, state, clock));
    std::string slug = whitespace.sub("-", title);
    slug = py::strip(unsafe.sub("", slug), "-._");
    slug = py::rstrip(py::slice(slug, 0, static_cast<std::int64_t>(kFileStemMaxTitle)), "-._");
    return slug.empty() ? stamp : stamp + "_" + slug;
}

} // namespace fv::meeting_state
