#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "base/signal.h"
#include "history/record_sync.h"

// The outbox that copies this computer's records to its paired host and reads them back
// (services/remote_records/sync.py's RecordSync), as History, Past Meetings, the Settings page and
// backups use it. The record-sync package implements it; without a paired host nothing provides
// it and History talks to history::NullRecordSync.
namespace fv::remote {

// A user-facing reason the host's records can't be reached right now.
class RecordsUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What the host keeps of one kind for this computer.
struct StoredCount {
    std::int64_t count = 0;
    std::int64_t bytes = 0;

    friend bool operator==(const StoredCount&, const StoredCount&) = default;
};

// What the storage section of Settings shows.
struct SyncStatus {
    std::string location = "local"; // one of RECORD_LOCATIONS
    bool paired = false;
    std::string host_name;
    // true: the host keeps records; false: its owner hasn't turned that on; nullopt: not known
    // yet, or a host too old to keep any.
    std::optional<bool> host_keeps;
    std::optional<bool> host_supports;
    int pending = 0;
    std::string waiting;
    std::string active;
    std::string error;
    // This computer's records on the host, by kind ("dictation", "meeting").
    std::map<std::string, StoredCount> stored;
    // Records kept here, by kind.
    std::map<std::string, std::int64_t> local_counts;

    friend bool operator==(const SyncStatus&, const SyncStatus&) = default;
};

// Held while a backup reads the database and the record folders: no record moves meanwhile.
// Releasing (destruction) lets the outbox go on. Move-only.
class SyncPause {
public:
    SyncPause() = default;
    explicit SyncPause(std::function<void()> release) : release_(std::move(release)) {}
    SyncPause(SyncPause&& other) noexcept : release_(std::exchange(other.release_, nullptr)) {}
    SyncPause& operator=(SyncPause&& other) noexcept {
        if (this != &other) {
            reset();
            release_ = std::exchange(other.release_, nullptr);
        }
        return *this;
    }
    SyncPause(const SyncPause&) = delete;
    SyncPause& operator=(const SyncPause&) = delete;
    ~SyncPause() { reset(); }

    void reset() {
        if (auto release = std::exchange(release_, nullptr)) {
            release();
        }
    }

private:
    std::function<void()> release_;
};

// Affinity: thread-safe. The record hooks (RecordSync's) only queue and return; location,
// remote_item and listing_wanted answer from memory and settings; every other method may wait on
// the host and runs off the GUI thread.
class RecordSyncControl : public RecordSync {
public:
    using RecordSync::cleared;
    // History (or Past Meetings) was cleared here except `keep` (a meeting still running): clear
    // it on the host too; the kept ones still go to the host later.
    virtual void cleared(std::string_view kind, std::span<const std::string> keep) = 0;

    virtual std::string location() = 0;
    // Where new records go; records already moved stay where they are. std::invalid_argument
    // "Unknown storage location: '<x>'" for anything but RECORD_LOCATIONS.
    virtual void set_location(std::string_view location) = 0;
    virtual SyncStatus status() = 0;
    // Whether new records wait for the owner's explicit transfer action.
    virtual bool manual_transfers() const { return false; }
    // Asks the host what it keeps for this computer (no answer: nothing changes).
    virtual void refresh_summary() = 0;
    // Queues every record kept only here, moved or copied as the location says; how many.
    virtual int send_existing() = 0;
    // Moves every record the host holds for this computer back here; how many. `progress` gets
    // "Bringing back N…".
    virtual int bring_back(std::function<void(std::string)> progress = {}) = 0;
    // Looks at the outbox soon; `now` also skips a retry wait and retries records that failed
    // too often to be tried on their own.
    virtual void wake(bool now = false) = 0;
    // This computer's records of `kind` on the host, newest first, each with "stored_on" (the
    // host's name). RecordsUnavailable with the reason when the host can't be asked right now.
    virtual std::vector<Json> list_remote(std::string_view kind, std::string_view query = "",
                                          int limit = 100) = 0;
    // The last listing's copy of one host record, for opening it.
    virtual std::optional<Json> remote_item(std::string_view kind, std::string_view record_id) = 0;
    // Deletes the host's copy now (History's delete of a host record); whether it was there.
    virtual bool delete_remote(std::string_view kind, std::string_view record_id) = 0;
    // A local path to a host-kept history entry's recording (cached until restart).
    // RecordsUnavailable when the entry has no recording there.
    virtual std::filesystem::path audio_for(std::string_view record_id) = 0;
    // Downloads a host record here to open it; `progress` gets (bytes so far, bytes in all).
    virtual void check_out(std::string_view kind, std::string_view record_id,
                           std::function<void(std::int64_t, std::int64_t)> progress = {}) = 0;
    // Explicit actions pin the peer whose row the user selected.
    virtual void check_out_from(std::string_view, std::string_view, std::string_view,
                                std::function<void(std::int64_t, std::int64_t)>) {
        throw RecordsUnavailable("This meeting can't be copied from the paired host.");
    }
    virtual void delete_remote_version(std::string_view, std::string_view, std::string_view,
                                       std::string_view) {
        throw RecordsUnavailable("This host cannot verify the selected meeting copy.");
    }
    virtual bool delete_remote_from(std::string_view, std::string_view, std::string_view) {
        throw RecordsUnavailable("This meeting can't be deleted from the paired host.");
    }
    // Which of `record_ids` the paired host also holds (kept on both).
    virtual std::set<std::string> copies_on_host(std::string_view kind,
                                                 std::span<const std::string> record_ids) = 0;
    // Whether History should ask the host: it may hold records of this computer's.
    virtual bool listing_wanted(std::string_view kind) = 0;
    // Stops record moves while a backup runs. std::runtime_error "Records are still moving to or
    // from the paired host. Try backup again after sync finishes." when a move doesn't end
    // within 30 s.
    virtual SyncPause paused_for_backup() = 0;

    // "records", "progress:<text>", and "" for any other change of status(); emitted from the
    // outbox's thread or the caller's.
    Signal<std::string> changed;
};

} // namespace fv::remote
