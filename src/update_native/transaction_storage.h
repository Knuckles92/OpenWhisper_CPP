#pragma once
#include <string_view>

#include "base/data_lease.h"
#include "update_native/guarded_directory.h"
#include "update_native/journal.h"

namespace fv::update_native {
// Construct from independently trusted context before reading a journal. Keep alive throughout
// the transaction, including the health wait. The lease is acquired separately because the
// replacement application needs its shared data lease while reporting health.
class TransactionStorage {
public:
    TransactionStorage(const JournalContext& context, std::string_view transaction_id);
    void acquire_data_lease();
    void release_data_lease();
    bool has_data_lease() const;
    // Both operations require the exclusive data lease. Input files stay pinned while copied;
    // destinations are published or deleted through no-follow handles.
    void snapshot_with_pinned_sources();
    void restore_with_pinned_sources();
    const JournalPaths& paths() const { return paths_; }
    GuardedDirectory& install_parent() { return install_parent_; }
    GuardedDirectory& transaction_directory() { return transaction_directory_; }

private:
    JournalPaths paths_;
    GuardedDirectory install_parent_;
    GuardedDirectory transaction_directory_;
    DataLease lease_;
};
} // namespace fv::update_native
