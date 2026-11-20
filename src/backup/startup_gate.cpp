#include "backup/startup_gate.h"

#include <format>
#include <mutex>
#include <optional>
#include <utility>

#include "backup/restore.h"

namespace fv::backup {

namespace {

// _PROCESS_LEASE's root: the data root this process was admitted to.
struct Admitted {
    std::mutex mutex;
    std::optional<std::filesystem::path> root;
};

Admitted& admitted() {
    static Admitted state;
    return state;
}

constexpr int kAttempts = 4;

} // namespace

DataLease acquire_startup_data_lease(const std::filesystem::path& root, const AppIdentity& app) {
    StartupRestoreSteps steps;
    steps.pending = [&app](const std::filesystem::path& path) {
        return pending_restore_exists(path, app);
    };
    steps.apply = [&app](const std::filesystem::path& path) {
        return apply_pending_restore(path, app);
    };
    return acquire_startup_data_lease(root, app, steps);
}

DataLease acquire_startup_data_lease(const std::filesystem::path& requested, const AppIdentity& app,
                                     const StartupRestoreSteps& steps) {
    const std::filesystem::path root = normalize_data_root(requested);
    Admitted& state = admitted();
    std::lock_guard lock(state.mutex);
    if (state.root) {
        if (*state.root != root) {
            throw StartupRestoreError(
                std::format("{} already holds a different data directory.", app.product));
        }
        return DataLease::acquire(root, app, LeaseMode::Shared);
    }
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        DataLease lease;
        try {
            lease = DataLease::acquire(root, app, LeaseMode::Shared);
        } catch (const DataLeaseBusy&) {
            throw StartupRestoreError(std::format("Another {} process is restoring this data. "
                                                  "Wait for it to finish, then launch again.",
                                                  app.product));
        }
        if (!steps.pending(root)) {
            state.root = root;
            return lease;
        }
        lease.release();

        DataLease exclusive;
        try {
            exclusive = DataLease::acquire(root, app, LeaseMode::Exclusive);
        } catch (const DataLeaseBusy&) {
            throw StartupRestoreError(std::format(
                "A backup restore is pending, but another {0} process is still using this data. "
                "Close every {0} window and History API process, then launch again to finish "
                "the restore.",
                app.product));
        }
        if (steps.pending(root) && !steps.apply(root)) {
            throw StartupRestoreError(
                "The pending backup restore was not applied. The existing data was left in "
                "place; launch again after resolving the restore error.");
        }
    }
    throw StartupRestoreError(
        "The backup restore changed repeatedly during startup; launch again.");
}

void release_startup_data_lease() {
    Admitted& state = admitted();
    std::lock_guard lock(state.mutex);
    state.root.reset();
}

} // namespace fv::backup
