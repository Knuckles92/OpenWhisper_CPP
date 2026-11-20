#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "db/database.h"

namespace fv::remote_records {

struct OutboxItem {
    std::string kind;
    std::string id;
    std::string host;
    std::string action;
    std::string state;
    std::string digest;
};

// One record_sync row per (kind, id). Transfers always select by pinned host, so a queued
// record cannot be silently retargeted when a different host is paired.
class Outbox {
public:
    Outbox(Database& db, Clock& clock) : db_(db), clock_(clock) {}
    bool enqueue(const OutboxItem& item);
    std::vector<OutboxItem> rows(std::string_view host);
    void complete(const OutboxItem& item);
    void fail(const OutboxItem& item, std::string_view reason);
    void forget(std::string_view kind, std::string_view id, std::string_view host);
    // A record kept on both computers already (fetched here, or shared): its row, done.
    void mark_done(const OutboxItem& item);
    // sync.py set_location: rows not yet sent follow a new storage choice. "local" drops them;
    // "host" makes them moves and "both" copies. Rows already done and deletions stay.
    void retarget(std::string_view location);

private:
    Database& db_;
    Clock& clock_;
};

} // namespace fv::remote_records
