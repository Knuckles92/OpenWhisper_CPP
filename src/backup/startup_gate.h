#pragma once

#include <filesystem>
#include <functional>

#include "base/data_lease.h"
#include "base/identity.h"

// services/backup_startup.py:140-194: the startup restore gate. Run once in main, before
// settings or SQLite open. Blocking: never on the GUI thread.
namespace fv::backup {

// The two restore calls the gate makes, replaceable in tests (OpenWhisper's tests monkeypatch
// them). Each gets the normalized root.
struct StartupRestoreSteps {
    std::function<bool(const std::filesystem::path& root)> pending; // pending_restore_exists
    std::function<bool(const std::filesystem::path& root)> apply;   // apply_pending_restore
};

// acquire_startup_data_lease: a pending restore applied offline under the exclusive lease, then
// the shared lease the process keeps until exit. Up to four rounds: a shared lease, the pending
// check under it, and when a restore is pending the exclusive lease and the restore. Root is
// normalize_data_root(root). StartupRestoreError with the sentence for each refusal:
//   another process holds the data exclusively: "Another <product> process is restoring this
//     data. Wait for it to finish, then launch again.";
//   a restore is pending and another process shares the data: "A backup restore is pending, but
//     another <product> process is still using this data. Close every <product> window and
//     History API process, then launch again to finish the restore.";
//   the restore returned false: "The pending backup restore was not applied. The existing data
//     was left in place; launch again after resolving the restore error.";
//   four rounds found a restore pending: "The backup restore changed repeatedly during startup;
//     launch again.";
//   a second call for another root of the same process: "<product> already holds a different
//     data directory.".
// A second call for the same root returns another shared lease on it without looking for a
// restore again (OpenWhisper returns the lease it keeps). Errors of the restore itself
// (BackupError, file errors) pass through, the data left as apply_pending_restore leaves it.
DataLease acquire_startup_data_lease(const std::filesystem::path& root, const AppIdentity& app);
DataLease acquire_startup_data_lease(const std::filesystem::path& root, const AppIdentity& app,
                                     const StartupRestoreSteps& steps);

// release_startup_data_lease: forgets the root the gate admitted, so the next call starts over.
// The caller releases the DataLease it holds.
void release_startup_data_lease();

} // namespace fv::backup
