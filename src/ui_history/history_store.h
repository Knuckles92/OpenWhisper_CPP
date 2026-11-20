#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "history/entry.h"

namespace fv {

class HistoryManager;

// The calls History's widgets make on services/history_manager.py's history_manager, behind
// one interface so their tests can stand in for it (OpenWhisper's tests patch the manager's
// methods). Names and meanings are HistoryManager's.
//
// Every method blocks on the database or the disk and runs on the io strand, never on the GUI
// thread, except get_recording_path: a file-exists check that cards and the dialog make while
// they build, as OpenWhisper's do.
// Affinity: thread-safe.
class HistoryStore {
public:
    virtual ~HistoryStore() = default;

    // Newest first; nullopt or 0 means every entry.
    virtual std::vector<HistoryEntry> get_history(std::optional<std::int64_t> limit) = 0;
    virtual std::vector<HistoryEntry> search(std::string_view query,
                                             std::optional<std::int64_t> limit) = 0;
    virtual std::optional<HistoryEntry> get_entry(std::string_view entry_id) = 0;
    virtual std::optional<HistoryEntry> last_dictation() = 0;
    // forget_dictation: a dictation deleted on the paired host is no longer the last one.
    virtual void forget_dictation(std::string_view /*entry_id*/) {}
    virtual std::optional<HistoryEntry> use_version(std::string_view entry_id,
                                                    std::string_view version) = 0;
    virtual bool delete_entry(std::string_view entry_id, bool delete_audio_file) = 0;
    virtual void clear_history() = 0;
    virtual void clear_history_and_recordings() = 0;
    // The saved recording's path when the file exists.
    virtual std::optional<std::filesystem::path> get_recording_path(std::string_view filename) = 0;
    virtual std::filesystem::path recordings_folder() const = 0;
};

// The store over the app's HistoryManager, which must outlive it.
std::shared_ptr<HistoryStore> make_history_store(HistoryManager& manager);

} // namespace fv
