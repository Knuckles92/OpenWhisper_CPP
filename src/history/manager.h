#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "base/paths.h"
#include "db/database.h"
#include "history/entry.h"
#include "history/record_sync.h"
#include "history/repository.h"

// services/history_manager.py: History writes, the saved-recordings folder and its retention,
// and the Original/AI versions of an entry. The host-kept entries of a paired computer
// (remote_history_entry) arrive with fv_remote.
namespace fv {

// The two versions of an entry AI cleanup changed (see entry_version).
inline constexpr std::string_view ORIGINAL_VERSION = "original";
inline constexpr std::string_view AI_VERSION = "ai";

// dictation, file, command, transform, ...: entry_kind when set, else "dictation" for a
// source_name starting "Quick Record" (Quick Record names every live dictation), else "file".
std::string kind_of(const HistoryEntry& entry);
// Which version entry.text is: ORIGINAL_VERSION, AI_VERSION, or "" when AI cleanup did not
// change the entry. An entry saved before cleaned_text existed shows the AI version until the
// first choice fills it in.
std::string entry_version(const HistoryEntry& entry);
// The AI's version of an entry AI cleanup changed, whichever is shown; nullopt otherwise.
std::optional<std::string> ai_text(const HistoryEntry& entry);
// This computer's own entry, kept here (no origin device, not stored on a host): only those
// can change.
bool is_local_entry(const HistoryEntry& entry);

// "20, 1.0 GB", "20", "all": the limits as the log names them.
std::string describe_retention(std::optional<std::int64_t> max_recordings,
                               std::optional<std::int64_t> max_bytes);

// A saved recording. timestamp is naive local ISO, from the file name
// (recording_YYYYMMDD_HHMMSS.wav) or else the file's modification time.
struct RecordingInfo {
    std::string filename;
    std::string timestamp;
    std::filesystem::path file_path;
    std::int64_t size_bytes = 0;

    std::string formatted_timestamp(const Clock& clock = system_clock()) const;
    friend bool operator==(const RecordingInfo&, const RecordingInfo&) = default;
};

// The recordings these limits would delete from `recordings` (newest first), newest first:
// beyond max_recordings, and past the first one whose running size exceeds max_bytes. The
// newest recording is always kept, even alone over the size cap.
std::vector<RecordingInfo> recordings_over_limit(const std::vector<RecordingInfo>& recordings,
                                                 std::optional<std::int64_t> max_recordings,
                                                 std::optional<std::int64_t> max_bytes);

// Retention limits; nullopt disables that limit.
struct RecordingRetention {
    std::optional<std::int64_t> max_recordings;
    std::optional<std::int64_t> max_bytes;
};

// What a transcription is saved from (HistoryManager.add_entry's arguments).
struct AddEntry {
    std::string text;
    std::string model;
    // Copied into the recordings folder when it exists.
    std::optional<std::filesystem::path> source_audio_path;
    std::optional<double> transcription_time; // seconds
    std::optional<double> audio_duration;     // seconds
    std::optional<std::int64_t> file_size;    // bytes
    std::optional<std::string> raw_text;
    std::optional<std::string> cleanup_provider;
    std::optional<std::string> cleanup_model;
    std::optional<std::string> source_name;
    // Context columns (HISTORY_CONTEXT_COLUMNS); any other name is dropped with a debug log.
    ContextFields columns;
    // For dictation_stats only, never saved: the words said (a snippet counted as its trigger)
    // and the job's counts ({"retranscription": true, "target": ..., "snippets": n, ...}). The
    // caller hands the same AddEntry and the saved entry to StatsRepository::record.
    std::optional<std::string> spoken_text;
    Json stats_counts = Json::object();
};

// A retry of a failed recording, saved in place (HistoryManager.complete_failed_recording).
struct FailedRecordingRetry {
    std::string text;
    std::string model;
    std::optional<double> transcription_time;
    std::optional<std::string> raw_text;
    std::optional<std::string> cleanup_provider;
    std::optional<std::string> cleanup_model;
    // Context columns; any other name throws py::TypeError.
    ContextFields context;
};

// What HistoryManager reaches beyond its database. Null members fall back to: the null record
// sync, the system clock, no live-event buffer, and dictations kept on this computer.
struct HistoryServices {
    RecordSync* record_sync = nullptr;
    // Asked at each save, edit, delete or clear when record_sync is null: the record sync the
    // app built once a host is paired (history_manager._record_sync, the lazy record_sync), or
    // null for none yet.
    std::function<RecordSync*()> find_record_sync;
    const Clock* clock = nullptr;
    // Drops the live events' replay buffer, which holds dictated text, so that a deleted
    // entry's words do not come back to a stream that reconnects (automation events.clear()).
    std::function<void()> forget_replayable_events;
    // Whether a dictation saved here now moves to the paired host after saving.
    std::function<bool()> dictations_move_to_host;
};

// Affinity: thread-safe; every method but the accessors blocks on the database or the disk,
// so never on the GUI thread.
class HistoryManager {
public:
    // The saved retention: resolve_max_saved_recordings(_bytes) of `settings`.
    HistoryManager(Database& database, std::filesystem::path recordings_folder,
                   const Json& settings, HistoryServices services = {});
    // Explicit limits: the whole policy, nothing read from settings.
    HistoryManager(Database& database, std::filesystem::path recordings_folder,
                   RecordingRetention retention, HistoryServices services = {});
    HistoryManager(const HistoryManager&) = delete;
    HistoryManager& operator=(const HistoryManager&) = delete;

    // config.RECORDINGS_FOLDER: <data root>/recordings.
    static std::filesystem::path default_recordings_folder(const Paths& paths);

    const std::filesystem::path& recordings_folder() const { return recordings_folder_; }
    std::optional<std::int64_t> max_recordings() const;
    std::optional<std::int64_t> max_bytes() const;
    HistoryRepository& repository() { return repository_; }

    // Applies retention limits now; nullopt disables that limit.
    void set_retention(std::optional<std::int64_t> max_recordings,
                       std::optional<std::int64_t> max_bytes);

    // Saves a transcription and, when its source audio exists, a copy of it in the recordings
    // folder (an entry whose copy fails is saved without audio). Keeps only context columns
    // with values; cleaned_text defaults to text when raw_text differs. A failed recording is
    // neither offered as the last dictation nor synced. The entry as saved: always set, since a
    // database failure throws (the caller reports its text, as Python's history save does).
    std::optional<HistoryEntry> add_entry(const AddEntry& fields);

    // The failed-recording entry whose saved audio is exactly `audio_path`.
    std::optional<HistoryEntry> failed_recording_for_path(const std::filesystem::path& audio_path);
    // Saves a retry in place, keeping the recording, its date, duration and source name.
    // Throws std::runtime_error("The failed recording entry is no longer in History") when the
    // entry is gone or no longer a local failed recording.
    HistoryEntry complete_failed_recording(std::string_view entry_id,
                                           const FailedRecordingRetry& retry);

    // Copies standalone audio (a failed quick transcription) into the recordings folder; its
    // file name, or nullopt when the file is missing or the copy failed.
    std::optional<std::string> preserve_recording(const std::filesystem::path& source_path);
    // Deletes the oldest recordings beyond the limits (history_manager._rotate_recordings).
    void rotate_recordings();
    // What these limits would delete, newest first; deletes nothing.
    std::vector<RecordingInfo> recordings_over_limit(std::optional<std::int64_t> max_recordings,
                                                     std::optional<std::int64_t> max_bytes);
    // (count, total bytes) of the *.wav files, cached for 2 s while the folder is unchanged.
    // A missing folder is (0, 0); std::filesystem::filesystem_error when it cannot be listed.
    std::pair<std::int64_t, std::int64_t> get_recordings_usage();
    // Saved recordings, newest first by timestamp.
    std::vector<RecordingInfo> get_recordings();

    // Newest first; a limit of 0 or none means every entry.
    std::vector<HistoryEntry> get_history(std::optional<std::int64_t> limit = std::nullopt);
    // search_history.
    std::vector<HistoryEntry> search(std::string_view query,
                                     std::optional<std::int64_t> limit = std::nullopt);
    // get_entry_by_id.
    std::optional<HistoryEntry> get_entry(std::string_view entry_id);

    // This computer's newest live dictation. One saved this session counts even after a move
    // to the paired host deleted its row here, marked stored_on "the host". Otherwise the
    // newest 50 local entries are scanned; nothing while dictations move to the host.
    std::optional<HistoryEntry> last_dictation();
    // Stops offering a deleted dictation as the last one.
    void forget_dictation(std::string_view entry_id);

    // Makes the entry show ORIGINAL_VERSION or AI_VERSION, never swapping fields: raw_text and
    // cleaned_text keep both and text becomes the chosen one. The entry as saved now, or
    // nullopt when it is not this computer's own or AI cleanup did not change it. Throws
    // py::ValueError("Unknown history entry version: '<version>'") for another version.
    std::optional<HistoryEntry> use_version(std::string_view entry_id, std::string_view version);

    // Deletes an entry and, when asked, its saved recording. kept_on_host: the paired host keeps
    // the entry now (a finished move), so it stays the last dictation.
    bool delete_entry(std::string_view entry_id, bool delete_audio_file = false,
                      bool kept_on_host = false);
    // This computer's entries; recordings stay.
    void clear_history();
    // Every saved recording first, then this computer's entries.
    void clear_history_and_recordings();

    // The saved recording's path when the file exists.
    std::optional<std::filesystem::path> get_recording_path(std::string_view filename);

private:
    HistoryManager(Database& database, std::filesystem::path recordings_folder,
                   RecordingRetention retention, HistoryServices services, bool from_settings);

    std::optional<std::string> save_recording(const std::filesystem::path& source_path);
    bool delete_recording_file(std::string_view filename);
    void invalidate_recordings_usage();
    void forget_all_dictations();
    void forget_replayable_events();
    RecordSync& record_sync();
    const Clock& clock() const;

    HistoryRepository repository_;
    std::filesystem::path recordings_folder_;
    HistoryServices services_;

    mutable std::mutex retention_mutex_;
    RecordingRetention retention_;

    std::mutex usage_mutex_;
    std::optional<std::pair<std::int64_t, std::int64_t>> usage_cache_;
    std::optional<std::filesystem::file_time_type> usage_signature_;
    std::chrono::steady_clock::time_point usage_checked_at_{};

    std::mutex newest_mutex_;
    // A copy of the newest dictation saved this session, so it stays the last dictation after a
    // move to the paired host deletes its row.
    std::optional<HistoryEntry> newest_dictation_;
};

} // namespace fv
