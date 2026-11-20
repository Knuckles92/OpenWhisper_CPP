#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "history/entry.h"
#include "history/manager.h"
#include "pyport/datetime.h"

// services/history_export.py: what a bulk History export writes. Selection and the documents
// are pure; write_per_entry_files is the only I/O and owns its folder. The export dialog owns
// threading and the path picking.
//
// An export entry is the dict serialize_history_entry makes (a JSON object): the 22 stored
// fields plus formatted_timestamp, preview_text and has_audio. The renderers read it as Python
// reads a dict, so a hand-made object with missing keys works as it does there.
// Affinity: thread-safe (no shared state); write_per_entry_files blocks on the disk.
namespace fv::history {

inline constexpr std::string_view FORMAT_MARKDOWN = "markdown";
inline constexpr std::string_view FORMAT_TXT = "txt";
inline constexpr std::string_view FORMAT_JSON = "json";
inline constexpr std::array<std::string_view, 3> EXPORT_FORMATS = {FORMAT_MARKDOWN, FORMAT_TXT,
                                                                   FORMAT_JSON};
// The "format_version" of a combined JSON export.
inline constexpr int kFormatVersion = 1;

// The fields a JSON export writes, in its order.
inline constexpr std::array<std::string_view, 22> JSON_FIELDS = {"id",
                                                                 "timestamp",
                                                                 "model",
                                                                 "text",
                                                                 "raw_text",
                                                                 "cleanup_provider",
                                                                 "cleanup_model",
                                                                 "audio_file",
                                                                 "transcription_time",
                                                                 "audio_duration",
                                                                 "file_size",
                                                                 "source_name",
                                                                 "title",
                                                                 "origin_device_id",
                                                                 "origin_device_name",
                                                                 "app_id",
                                                                 "app_name",
                                                                 "app_category",
                                                                 "cleanup_level",
                                                                 "entry_kind",
                                                                 "cleaned_text",
                                                                 "language"};

// The default single-file name before its extension: "<product in lower case>_history" (the
// dialog adds ".md", ".txt" or ".json").
std::string default_export_file_stem();

// A History row as an export entry: the 22 fields (missing text fields as "", the others as
// null) plus formatted_timestamp (in the clock's zone; "" without a timestamp), preview_text
// and has_audio.
Json serialize_history_entry(const HistoryEntry& entry, const Clock& clock = system_clock());

// Every History entry, newest first, serialized for the export dialog. Blocking.
std::vector<Json> list_export_entries(HistoryManager& manager, const Clock& clock = system_clock());
// The same over entries already read, in the order given.
std::vector<Json> list_export_entries(const std::vector<HistoryEntry>& history,
                                      const Clock& clock = system_clock());

// The export criteria. from and to are naive local wall times, both ends inclusive; stored
// timestamps are read in the clock's zone. With either bound set, an entry whose timestamp
// cannot be read is left out. Throws as entry_local_time does.
std::vector<Json> filter_export_entries(const std::vector<Json>& entries,
                                        const std::optional<py::DateTime>& from_dt = std::nullopt,
                                        const std::optional<py::DateTime>& to_dt = std::nullopt,
                                        bool only_with_audio = false,
                                        const Clock& clock = system_clock());

struct ExportOptions {
    // Both apply to Markdown only: a plain transcript is the transcript by definition and JSON
    // is the complete record.
    bool include_cleaned = true;
    bool include_raw = true;
};

// One entry as a document: JSON (indent 2, non-ASCII kept) for "json", text for "txt",
// Markdown for anything else.
std::string render_entry_document(const Json& entry, std::string_view format,
                                  const ExportOptions& options = {});

// Every entry as one document. JSON is an envelope {format_version, exported_at (local time,
// to the second), entries}; text joins the entries with a line of 60 dashes; Markdown opens with
// a header stamped with the local time. Times come from the clock.
std::string render_export_document(const std::vector<Json>& entries, std::string_view format,
                                   const ExportOptions& options = {},
                                   const Clock& clock = system_clock());

// Writes one "<stem>.<format>" file per entry into out_dir (made with its parents when missing),
// so Markdown files end ".markdown". An existing file is never replaced: the name takes "-2",
// "-3", ... and a name already used by this call is skipped the same way. Files are UTF-8 with
// the platform's text line endings (CRLF on Windows, as Python's text mode writes them). Returns
// the paths written, in order. Throws std::filesystem::filesystem_error when a folder or file
// cannot be made or written; files written before that stay.
std::vector<std::filesystem::path> write_per_entry_files(const std::vector<Json>& entries,
                                                         std::string_view format,
                                                         const std::filesystem::path& out_dir,
                                                         const ExportOptions& options = {},
                                                         const Clock& clock = system_clock());

// "YYYYMMDD_HHMMSS_<slug>" from the entry's local time ("unknown_date" without one) and its
// preview (or text): whitespace runs become "-", anything but \w, "-" and "." is dropped, the
// ends lose "-", "." and "_", and at most 60 code points are kept. Just the stamp when nothing
// is left. Throws as entry_local_time does.
std::string entry_file_stem(const Json& entry, const Clock& clock = system_clock());

// history_export._as_local: a stored timestamp ("Z" accepted) as local wall time, naive; aware
// values are converted to the clock's zone. nullopt for an empty or unreadable value. Like
// Python's astimezone() on Windows it throws py::OverflowError("date value out of range") when
// the moment leaves years 1..9999 in UTC, and py::PyError of type "OSError" ("[Errno 22]
// Invalid argument") outside the C runtime's localtime range (1970 to 3001-01-19).
std::optional<py::DateTime> entry_local_time(const Json& timestamp, const Clock& clock);

// What the cleanup of an entry is called: "<provider> · <model>", "<model>" or "Cleaned", and
// " (original kept)" when Undo AI edit chose the original (text equals raw_text).
std::string cleanup_label(const Json& entry);
// Whether the entry was cleaned up: it has a cleanup model or a raw text.
bool was_cleaned(const Json& entry);

} // namespace fv::history
