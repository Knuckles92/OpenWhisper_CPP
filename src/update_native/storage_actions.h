#pragma once
#include "download/verified_files.h"
#include "update_native/transaction_storage.h"
#include <chrono>
#include <string_view>

namespace fv::update_native {
// How long a file operation waits out a lock (_LOCK_WAIT_S), and how it polls. A process that
// has just exited keeps its image mapped for a moment and scanners open files they have never
// seen; both surface as a sharing violation on the install directory.
struct LockRetry {
    std::chrono::milliseconds timeout{std::chrono::seconds(60)};
    download::LockRetryOptions options{};
};

// Filesystem actions for a transaction whose trees have already been authenticated by its
// platform. The parent guard remains held throughout; each rename refuses an occupied target and
// is retried while the system reports a lock; any other error fails at once.
class StorageActions {
public:
    StorageActions(const JournalContext&, std::string_view transaction_id);
    TransactionStorage& storage() { return storage_; }
    // Tests replace the wait and what counts as a lock.
    void set_lock_retry(LockRetry retry) { retry_ = std::move(retry); }
    void move_old_to_rollback();
    void move_candidate_to_install();
    void move_new_to_failed();
    void move_rollback_to_install();

private:
    void move_tree(const std::filesystem::path& from, const std::filesystem::path& to);
    TransactionStorage storage_;
    std::filesystem::path failed_name_;
    LockRetry retry_;
};
} // namespace fv::update_native
