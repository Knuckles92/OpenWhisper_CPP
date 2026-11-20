#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "meeting_state/repository.h"
#include "pyport/datetime.h"

// meeting/export/bulk.py: the Past Meetings export: which meetings, and the documents made of
// them. Rendering is pure; write_per_meeting_files is the only I/O and owns its folder. The
// export dialog owns threading and path picking.
// Affinity: thread-safe; the repository and file functions block.
namespace fv::meeting_state {

inline constexpr std::string_view FORMAT_MARKDOWN = "markdown";
inline constexpr std::string_view FORMAT_TXT = "txt";
inline constexpr std::string_view FORMAT_JSON = "json";
inline constexpr std::array<std::string_view, 3> EXPORT_FORMATS = {FORMAT_MARKDOWN, FORMAT_TXT,
                                                                   FORMAT_JSON};

// The renderers' inputs for one meeting.
struct MeetingExport {
    Json meeting;               // the row
    Json state;                 // stored_state_dict(row): {} without a readable snapshot
    std::vector<Json> segments; // get_segments, by start_s
};

// Both toggles apply to Markdown only: the txt format is the transcript by definition and the
// JSON export is the complete record.
struct BulkExportOptions {
    bool include_transcript = true;
    bool include_intelligence = true;
};

// Past meetings, newest first, without the live ones (active, paused, ending); a row without a
// content_summary gets summarize_meeting_content's, so the transcript filter reads the row
// alone.
std::vector<Json> list_export_meetings(MeetingRepository& repository);

// The export criteria. from and to are naive local wall times (a date picker's), both ends
// inclusive; stored times are converted to the clock's zone first. With a bound set, a meeting
// whose started_at does not parse is left out; only_with_transcript drops only the meetings
// known to have none (content_summary.has_transcript false). Live meetings are always left out.
std::vector<Json> filter_export_meetings(const std::vector<Json>& meetings,
                                         const std::optional<py::DateTime>& from_dt = std::nullopt,
                                         const std::optional<py::DateTime>& to_dt = std::nullopt,
                                         bool only_with_transcript = false,
                                         const Clock& clock = system_clock());

// The inputs for one meeting; nullopt when it no longer exists (deleted while an export queue
// was being worked through).
std::optional<MeetingExport> collect_meeting_export(MeetingRepository& repository,
                                                    std::string_view meeting_id);

// One meeting as a document: JSON for "json", the transcript for "txt", Markdown otherwise.
std::string render_meeting_document(const MeetingExport& entry, std::string_view format,
                                    const BulkExportOptions& options = {},
                                    const Clock& clock = system_clock());

// Every entry as one document (bulk.py render_export_document). JSON is an envelope
// {format_version, exported_at (local time, to the second), meetings}; text joins the
// transcripts with a line of 60 "="; Markdown opens with a header stamped with the local time.
std::string render_combined(const std::vector<MeetingExport>& entries, std::string_view format,
                            const BulkExportOptions& options = {},
                            const Clock& clock = system_clock());

// Writes one "<stem>.<format>" file per entry into dir (made with its parents when missing), so
// per-meeting Markdown files end ".markdown" (single-file saves use ".md"). An existing file is
// never replaced: the name takes "-2", "-3", ... and a name this call already used is skipped
// the same way. UTF-8 with the platform's text line endings (CRLF on Windows, as Python's text
// mode writes them). Returns the paths written, in order. Throws
// std::filesystem::filesystem_error when the folder or a file cannot be made or written; files
// written before that stay.
std::vector<std::filesystem::path>
write_per_meeting_files(const std::filesystem::path& dir, std::string_view format,
                        const std::vector<MeetingExport>& entries,
                        const BulkExportOptions& options = {}, const Clock& clock = system_clock());

// Filesystem-safe "YYYYMMDD_HHMMSS_<title>" ("unknown_date" without a start time): whitespace
// runs become "-", anything but \w, "-" and "." is dropped, the ends lose "-", "." and "_", and
// at most 60 code points of title are kept.
std::string meeting_file_stem(const Json& meeting, const Json& state,
                              const Clock& clock = system_clock());

} // namespace fv::meeting_state
