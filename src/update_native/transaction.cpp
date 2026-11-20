#include "update_native/transaction.h"

#include <utility>

#include "base/identity.h"
#include "update_native/manifest.h"

namespace fv::update_native {
std::string parent_still_running_message() {
    const std::string product(identity().product);
    return product +
           " did not close, so the update was not installed. Your current version is unchanged "
           "\xE2\x80\x94 close " +
           product + " completely and try the update again.";
}
std::string application_running_message() {
    return std::string(identity().product) + " is still running.";
}
namespace {
void checkpoint(const JournalContext& context, const JournalPaths& paths, UpdateJournal& journal,
                TransactionState state, bool cleanup, TransactionPlatform& platform) {
    UpdateJournal next = journal;
    next.state = state;
    next.cleanup_rollback = cleanup;
    (void)validate_journal_binding(context, next, next.transaction_id);
    platform.persist(paths, next);
    journal = std::move(next);
}
void notify(const TransactionHook& hook, std::string_view point) {
    if (hook)
        hook(point);
}
void check_cancel(const CancelToken& cancel) {
    if (cancel.cancelled())
        throw Canceled("The update was canceled.");
}

TransactionState reconcile(const JournalContext& context, const JournalPaths& paths,
                           UpdateJournal& journal, TransactionPlatform& platform) {
    if (journal.state == TransactionState::RolledBack ||
        journal.state == TransactionState::Superseded)
        return journal.state;
    platform.verify_registration(paths, journal);
    const TreeState active = platform.active_tree(paths, journal);
    const TreeState rollback = platform.rollback_tree(paths, journal);
    if (rollback != TreeState::Missing && rollback != TreeState::Old)
        throw UpdateError("The interrupted update has an invalid rollback tree.");
    if (journal.state == TransactionState::Preparing &&
        (active != TreeState::Old || rollback != TreeState::Missing))
        throw UpdateError("The unstarted update has unexpected installation trees.");
    if (journal.state == TransactionState::Prepared &&
        (active == TreeState::New || active == TreeState::DamagedNew))
        throw UpdateError("The prepared update has unexpected installation trees.");
    const bool health = journal.state == TransactionState::Healthy ||
                        ((journal.state == TransactionState::OldMoved ||
                          journal.state == TransactionState::NewActive) &&
                         platform.health_confirmed(paths, journal));
    if (active == TreeState::New && health) {
        platform.update_registration(paths, journal);
        if (journal.state != TransactionState::Healthy)
            checkpoint(context, paths, journal, TransactionState::Healthy, true, platform);
        platform.clear_recovery(paths, journal);
        return journal.state;
    }
    if (active == TreeState::Foreign) {
        // A later setup or repair owns the active tree. Never replace it with our rollback.
        checkpoint(context, paths, journal, TransactionState::Superseded,
                   rollback == TreeState::Old || journal.cleanup_rollback, platform);
        platform.clear_recovery(paths, journal);
        return journal.state;
    }
    if (active == TreeState::Old) {
        // This includes a crash after persisting rename intent but before the first rename.
        // NewActive with no rollback also covers a crash after moving the rollback tree back,
        // but before restoring the snapshot. That restore must be repeatable while gated.
        if (journal.state == TransactionState::NewActive && rollback == TreeState::Missing &&
            !health) {
            platform.restore_data(paths, journal);
            platform.restore_registration(paths, journal);
        }
        const auto final_state = journal.state == TransactionState::Preparing ||
                                         journal.state == TransactionState::Prepared
                                     ? TransactionState::Superseded
                                     : TransactionState::RolledBack;
        checkpoint(context, paths, journal, final_state,
                   rollback == TreeState::Old || journal.cleanup_rollback, platform);
        platform.clear_recovery(paths, journal);
        return journal.state;
    }
    if (rollback != TreeState::Old)
        throw UpdateError("The interrupted update has no validated rollback tree.");
    if (active == TreeState::New || active == TreeState::DamagedNew)
        platform.move_new_to_failed(paths);
    // Missing is also possible between the two persisted renames. The platform's exclusive
    // rename refuses a newly appeared install instead of overwriting it.
    platform.move_rollback_to_install(paths);
    if (platform.active_tree(paths, journal) != TreeState::Old)
        throw UpdateError("The restored installation could not be verified.");
    if (!health)
        platform.restore_data(paths, journal);
    platform.restore_registration(paths, journal);
    checkpoint(context, paths, journal, TransactionState::RolledBack, true, platform);
    platform.clear_recovery(paths, journal);
    return journal.state;
}
} // namespace

TransactionState apply_prepared(const JournalContext& context, UpdateJournal& journal,
                                TransactionPlatform& platform, const CancelToken& cancel,
                                TransactionHook hook) {
    const auto paths = validate_journal_binding(context, journal, journal.transaction_id);
    if (journal.state != TransactionState::Prepared)
        throw UpdateError("The update transaction is not prepared.");
    auto gates = platform.acquire_gates();
    if (!gates)
        throw UpdateError("Another update or setup is already in progress.");
    check_cancel(cancel);
    if (journal.parent_pid != 0 && !platform.parent_exited(paths, journal, cancel))
        throw UpdateError(parent_still_running_message());
    check_cancel(cancel);
    platform.verify_registration(paths, journal);
    if (platform.active_tree(paths, journal) != TreeState::Old)
        throw UpdateError("The installed application has changed.");
    if (platform.candidate_tree(paths, journal) != TreeState::New)
        throw UpdateError("The prepared update could not be verified.");
    if (platform.rollback_tree(paths, journal) != TreeState::Missing)
        throw UpdateError("An update rollback already exists.");
    check_cancel(cancel);
    platform.snapshot_data(paths, journal);
    check_cancel(cancel);
    platform.install_recovery(paths, journal);
    bool durable_intent = false;
    bool health_confirmed = false;
    bool launched = false;
    try {
        checkpoint(context, paths, journal, TransactionState::OldMoved, true, platform);
        durable_intent = true;
        notify(hook, "after_old_moved_intent");
        check_cancel(cancel);
        platform.move_old_to_rollback(paths);
        notify(hook, "after_old_moved");
        checkpoint(context, paths, journal, TransactionState::NewActive, true, platform);
        notify(hook, "after_new_active_intent");
        check_cancel(cancel);
        platform.move_candidate_to_install(paths);
        notify(hook, "after_new_active");
        // The candidate can change between the pre-move check and the rename. Never execute it
        // until its contents have been authenticated again at the active destination.
        if (platform.active_tree(paths, journal) != TreeState::New)
            throw UpdateError("The installed update failed verification after activation.");
        check_cancel(cancel);
        notify(hook, "before_health");
        launched = true;
        if (!platform.launch_and_check_health(paths, journal, cancel))
            throw UpdateError("The new version started but did not become ready.");
        health_confirmed = true;
        notify(hook, "after_health");
        platform.update_registration(paths, journal);
        checkpoint(context, paths, journal, TransactionState::Healthy, true, platform);
        platform.clear_recovery(paths, journal);
        return journal.state;
    } catch (const std::exception& original) {
        if (health_confirmed)
            throw UpdateError("The new version is running, but update finalization failed: " +
                              std::string(original.what()));
        try {
            if (launched)
                platform.stop_failed_health_process(paths, journal);
            if (durable_intent)
                (void)reconcile(context, paths, journal, platform);
            else
                platform.clear_recovery(paths, journal);
        } catch (const std::exception& rollback) {
            throw UpdateError(
                "The update failed and rollback could not restore the previous installation: " +
                std::string(rollback.what()));
        }
        throw;
    }
}
TransactionState recover_transaction(const JournalContext& context, UpdateJournal& journal,
                                     TransactionPlatform& platform) {
    const auto paths = validate_journal_binding(context, journal, journal.transaction_id);
    auto gates = platform.acquire_gates();
    if (!gates)
        throw UpdateError("Another update or setup is already in progress.");
    return reconcile(context, paths, journal, platform);
}
} // namespace fv::update_native
