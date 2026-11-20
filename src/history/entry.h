#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "base/clock.h"

// services/models.py TranscriptionHistory: one History entry as a value (SQLAlchemy's
// expire_on_commit=False rows stay readable after their session; here they are plain copies).
namespace fv {

// services/models.py FAILED_RECORDING_KIND.
inline constexpr std::string_view FAILED_RECORDING_KIND = "failed_recording";

// services/models.py HISTORY_CONTEXT_COLUMNS: the optional per-entry columns a caller may set
// when saving an entry.
inline constexpr std::array<std::string_view, 7> HISTORY_CONTEXT_COLUMNS = {
    "app_id",     "app_name",     "app_category", "cleanup_level",
    "entry_kind", "cleaned_text", "language"};

// Keyword arguments naming context columns, as Python's **context: a name with no value is
// None. Kept loose so that a caller can pass a job's fields without vetting them.
using ContextFields = std::map<std::string, std::optional<std::string>, std::less<>>;

// The 22 columns of transcription_history, plus the transient fields History gives entries a
// paired host keeps (never stored here).
struct HistoryEntry {
    std::string id;
    std::string text;
    std::optional<std::string> raw_text;
    std::string timestamp; // ISO 8601; aware UTC for new rows, naive local wall time for old ones
    std::string model;
    std::optional<std::string> audio_file;    // a file name in the recordings folder
    std::optional<double> transcription_time; // seconds
    std::optional<double> audio_duration;     // seconds
    std::optional<std::int64_t> file_size;    // bytes
    std::optional<std::string> cleanup_provider;
    std::optional<std::string> cleanup_model;
    std::optional<std::string> source_name;
    std::optional<std::string> title;
    std::optional<std::string> origin_device_id;
    std::optional<std::string> origin_device_name;
    std::optional<std::string> app_id;
    std::optional<std::string> app_name;
    std::optional<std::string> app_category;
    std::optional<std::string> cleanup_level;
    std::optional<std::string> entry_kind; // NULL on legacy rows: the kind follows source_name
    std::optional<std::string> cleaned_text;
    std::optional<std::string> language;

    // Transient: the paired host that keeps the entry, the host that also keeps a copy, and
    // whether the host kept the recording.
    std::optional<std::string> stored_on;
    std::optional<std::string> also_on;
    bool remote_audio = false;

    std::string formatted_timestamp(const Clock& clock = system_clock()) const;
    std::string preview_text() const;

    // The context column `name` (one of HISTORY_CONTEXT_COLUMNS), or nullptr.
    std::optional<std::string>* context_column(std::string_view name);
    const std::optional<std::string>* context_column(std::string_view name) const;

    friend bool operator==(const HistoryEntry&, const HistoryEntry&) = default;
};

// TranscriptionHistory.preview_text: the text stripped, "Empty transcript" when nothing is
// left, else at most 100 code points, cut back to the last space inside them and ended with
// "..." when longer (all 100 kept when they hold no space).
std::string preview_text(std::string_view text);

// models.history_context: `values` without its Nones. Throws py::TypeError("Not history context
// columns: a, b") naming every key that is not a context column, sorted.
ContextFields history_context(const ContextFields& values);

// The arguments of TranscriptionHistory.create.
struct NewEntry {
    std::string text;
    std::string model;
    std::optional<std::string> audio_file;
    std::optional<double> transcription_time;
    std::optional<double> audio_duration;
    std::optional<std::int64_t> file_size;
    std::optional<std::string> raw_text;
    std::optional<std::string> cleanup_provider;
    std::optional<std::string> cleanup_model;
    std::optional<std::string> source_name;
    ContextFields context;
};

// TranscriptionHistory.create: a str(uuid4()) id and datetime.now(timezone.utc).isoformat()
// ("2026-03-15T14:30:00.123456+00:00"). Throws as history_context does.
HistoryEntry create_entry(const NewEntry& fields, const Clock& clock = system_clock());

// str(uuid.uuid4()): 36 characters, lowercase hex with dashes.
std::string uuid4_string();

} // namespace fv
