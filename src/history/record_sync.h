#pragma once

#include <string_view>

// The outbox that copies records to a paired host (services/remote_records/sync.py's
// record_sync, as history_manager._record_sync() reaches it). fv_remote implements it; without
// a paired host History talks to NullRecordSync.
// Affinity: thread-safe; called from whichever thread saves or changes History.
namespace fv {

class RecordSync {
public:
    virtual ~RecordSync() = default;
    // kind is the record kind ("dictation" for History entries).
    virtual void record_saved(std::string_view kind, std::string_view record_id) = 0;
    virtual void record_edited(std::string_view kind, std::string_view record_id) = 0;
    virtual void record_deleted(std::string_view kind, std::string_view record_id) = 0;
    virtual void cleared(std::string_view kind) = 0;
};

// Records nothing: no paired host.
class NullRecordSync final : public RecordSync {
public:
    void record_saved(std::string_view, std::string_view) override {}
    void record_edited(std::string_view, std::string_view) override {}
    void record_deleted(std::string_view, std::string_view) override {}
    void cleared(std::string_view) override {}
};

// One shared instance.
RecordSync& null_record_sync();

} // namespace fv
