#include "update_native/transaction_runner.h"

#include <exception>
#include <string>
#include <system_error>

#include "base/log.h"
#include "base/utf8.h"
#include "pyport/text.h"
#include "update/release_identity.h"
#include "update_native/pinned_tree.h"
#include "update_native/terminal_cleanup.h"

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;
constexpr log::Logger kLog("services.app_update_apply");

bool terminal(TransactionState state) {
    return state == TransactionState::Healthy || state == TransactionState::RolledBack ||
           state == TransactionState::Superseded;
}

void collect(const JournalContext& context, const RunnerActions& actions,
             const UpdateJournal& journal) {
    if (!actions.cleanup || !terminal(journal.state))
        return;
    try {
        actions.cleanup(journal);
    } catch (const std::exception& error) {
        kLog.info("Update completed; cleanup will retry on next start: {}", error.what());
    }
    // _remove_transaction_payloads: the archive goes with the trees, whatever became of them.
    try {
        remove_transaction_archive(context, journal);
    } catch (const std::exception& error) {
        kLog.info("Update completed; cleanup will retry on next start: {}", error.what());
    }
}

// Returns whether the message was left for the next launch.
bool report(const RunnerActions& actions, std::string_view message) {
    if (!actions.report)
        return false;
    try {
        actions.report(message);
        return true;
    } catch (const std::exception& error) {
        kLog.warning("Could not leave the update error for the next launch: {}", error.what());
        return false;
    }
}

// os.path.normcase(os.path.normpath(os.path.abspath(path))) is all _is_within compares.
std::string comparable(const fs::path& path) {
    std::error_code error;
    auto absolute = fs::absolute(path, error);
    if (error)
        absolute = path;
    auto text = path_to_utf8(absolute.lexically_normal());
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\'))
        text.pop_back();
#ifdef _WIN32
    text = py::lower(text);
#endif
    return text;
}

bool is_within(const fs::path& path, const fs::path& root) {
    const auto inner = comparable(path);
    const auto outer = comparable(root);
    if (inner == outer)
        return true;
    if (!inner.starts_with(outer) || inner.size() <= outer.size())
        return false;
    const char next = inner[outer.size()];
    return next == '/' || next == '\\';
}
} // namespace

TransactionState run_transaction(const JournalContext& context, UpdateJournal& journal,
                                 TransactionPlatform& platform, bool recover,
                                 const RunnerActions& actions, const CancelToken& cancel) {
    const auto before = journal.state;
    try {
        if (recover)
            (void)recover_transaction(context, journal, platform);
        else
            (void)apply_prepared(context, journal, platform, cancel);
    } catch (const std::exception& error) {
        const std::string message = error.what();
        kLog.error("The update transaction failed in state {}: {}", to_string(journal.state),
                   message);
        const bool reported = report(actions, message);
        // The rollback is complete: its leftovers go, and the previous version comes back so the
        // user is not left without the application while reading why.
        const bool rolled_back = !recover && journal.state == TransactionState::RolledBack;
        if (rolled_back)
            collect(context, actions, journal);
        if (rolled_back && actions.relaunch) {
            bool relaunched = false;
            try {
                actions.relaunch(journal);
                relaunched = true;
            } catch (const std::exception& failure) {
                kLog.error("The previous version could not be started: {}", failure.what());
            }
            // The application shows only what was left for it: without that the helper's own
            // box is the user's only report of the failure.
            if (relaunched && reported)
                throw ReportedByApplication(message);
        }
        throw;
    }
    if (recover && journal.state == TransactionState::RolledBack &&
        before != TransactionState::RolledBack)
        report(actions, kInterruptedRollbackMessage);
    collect(context, actions, journal);
    if (recover && actions.relaunch &&
        (journal.state == TransactionState::RolledBack ||
         journal.state == TransactionState::Healthy))
        actions.relaunch(journal);
    return journal.state;
}

void relaunch_verified(const JournalContext& context, const UpdateJournal& journal,
                       const std::function<bool(const fs::path&)>& start) {
    const auto paths = validate_journal_binding(context, journal, journal.transaction_id);
    PinnedTree active(paths.install_dir);
    if (journal.state == TransactionState::Healthy)
        active.verify_release(journal.new_version, journal.new_exe_sha256,
                              journal.compatibility_files);
    else
        active.verify_old(journal.old_exe_sha256);
    const auto executable = paths.install_dir / path_from_utf8(update::release_identity().app_exe);
    if (!start || !start(executable))
        throw UpdateError("The recovered application could not be started.");
}

bool leave_launch_directory(const fs::path& updates_root) {
    std::error_code error;
    fs::create_directories(updates_root, error);
    if (error) {
        kLog.warning("Could not create {}: {}", path_to_utf8(updates_root), error.message());
        return false;
    }
    const auto started_in = fs::current_path(error);
    if (!error && is_within(started_in, updates_root))
        return false;
    fs::current_path(updates_root, error);
    if (error) {
        kLog.warning("Could not change directory to {}: {}", path_to_utf8(updates_root),
                     error.message());
        return false;
    }
    return true;
}
} // namespace fv::update_native
