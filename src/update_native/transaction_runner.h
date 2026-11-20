#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

#include "base/cancel.h"
#include "update_native/journal.h"
#include "update_native/manifest.h"
#include "update_native/transaction.h"

// One run of the updater helper around a transaction (scripts/updater_helper.py: main;
// services/app_update_apply.py: _CommitAttempt.roll_back, recover_transaction's relaunch,
// leave_launch_directory). The platform, the cleanup and the launches are the caller's:
// win/installed_transaction.cpp supplies the real ones, tests supply fakes.
namespace fv::update_native {

// What happens around the transaction. Any of them may be empty.
struct RunnerActions {
    // Removes what a finished transaction left on disk (the rollback tree of a healthy update,
    // the candidate of a retired one). It may throw: cleanup never changes the outcome, and
    // whatever stays is collected by a later start.
    std::function<void(const UpdateJournal&)> cleanup;
    // Starts the application that is installed now: the previous version after a rollback, the
    // new one after a recovery that confirmed it. Throws when it cannot.
    std::function<void(const UpdateJournal&)> relaunch;
    // Leaves a message for the application's next launch (apply_error.txt), which shows it in
    // its "Update failed" box.
    std::function<void(std::string_view)> report;
};

// A failure the relaunched application reports itself: the helper stays silent about it.
class ReportedByApplication : public UpdateError {
public:
    using UpdateError::UpdateError;
};

// "The previous update was interrupted and was rolled back."
inline constexpr std::string_view kInterruptedRollbackMessage =
    "The previous update was interrupted and was rolled back.";

// Applies a prepared journal, or recovers an interrupted one, and returns the final state.
// - A failure is reported for the next launch and rethrown. When the commit was rolled back
//   the previous version is started again first, so it is running when the user reads why;
//   the failure then leaves as ReportedByApplication, unless the message could not be left for
//   it (then the helper reports it itself).
// - A recovery that had to roll back reports kInterruptedRollbackMessage and starts the
//   previous version; one that confirmed the new version starts that.
// - Cleanup runs after the outcome is decided and cannot change it.
TransactionState run_transaction(const JournalContext&, UpdateJournal&, TransactionPlatform&,
                                 bool recover, const RunnerActions&, const CancelToken& = {});

// Starts the installed application only once the tree it runs from is authenticated: the new
// release after a healthy update, the previous executable otherwise. `start` returns false
// when the program could not be started. Throws UpdateError.
void relaunch_verified(const JournalContext&, const UpdateJournal&,
                       const std::function<bool(const std::filesystem::path&)>& start);

// Gets the helper out of the folder it was started in (leave_launch_directory, without the
// PyInstaller restart a native program does not need). A Windows process holds its current
// directory open without delete sharing, so a helper started inside the install folder could
// never move it aside; the application starts it from the transaction folder, but the helper
// must not depend on its launcher. Creates `updates_root` and changes to it unless the current
// directory is already inside it. True when the directory was changed; a failure is logged and
// answered false.
bool leave_launch_directory(const std::filesystem::path& updates_root);

} // namespace fv::update_native
