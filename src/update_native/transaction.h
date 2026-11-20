#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "update_native/journal.h"

namespace fv::update_native {

// A platform classifies a tree only after checking its manifest or old executable digest.
enum class TreeState { Missing, Old, New, DamagedNew, Foreign };
class TransactionGates {
public:
    virtual ~TransactionGates() = default;
};

// The transaction algorithm has no production filesystem or process implementation. A real
// adapter must hold the setup, update and application gates; bind all operations to verified
// directory handles with no reparse traversal; use exclusive-destination renames; recheck the
// registry and process identity; and compare the complete health token. It must not act on paths
// read from the journal. Methods receive JournalPaths independently derived from JournalContext.
class TransactionPlatform {
public:
    virtual ~TransactionPlatform() = default;
    virtual std::unique_ptr<TransactionGates> acquire_gates() = 0;
    // True only when the independently checked parent identity exited; never terminate an
    // unidentified or recycled PID. A PID of zero has no parent to wait for.
    virtual bool parent_exited(const JournalPaths&, const UpdateJournal&, const CancelToken&) = 0;
    virtual void verify_registration(const JournalPaths&, const UpdateJournal&) = 0;
    virtual TreeState active_tree(const JournalPaths&, const UpdateJournal&) = 0;
    virtual TreeState candidate_tree(const JournalPaths&, const UpdateJournal&) = 0;
    virtual TreeState rollback_tree(const JournalPaths&, const UpdateJournal&) = 0;
    virtual void snapshot_data(const JournalPaths&, const UpdateJournal&) = 0;
    virtual void restore_data(const JournalPaths&, const UpdateJournal&) = 0;
    virtual void install_recovery(const JournalPaths&, const UpdateJournal&) = 0;
    virtual void clear_recovery(const JournalPaths&, const UpdateJournal&) = 0;
    // Persist and flush the whole bound journal before returning. A failed write leaves the old
    // durable state in place; no caller may advance a rename past a failed transition.
    virtual void persist(const JournalPaths&, const UpdateJournal&) = 0;
    virtual void move_old_to_rollback(const JournalPaths&) = 0;
    virtual void move_candidate_to_install(const JournalPaths&) = 0;
    // Preserve a new active tree at a fresh failed-tree name; refuse an existing destination.
    virtual void move_new_to_failed(const JournalPaths&) = 0;
    virtual void move_rollback_to_install(const JournalPaths&) = 0;
    // The adapter releases the app gate for the new process while retaining setup/update gates.
    // False covers timeout, process exit, invalid token or cancellation.
    virtual bool launch_and_check_health(const JournalPaths&, const UpdateJournal&,
                                         const CancelToken&) = 0;
    virtual bool health_confirmed(const JournalPaths&, const UpdateJournal&) = 0;
    virtual void update_registration(const JournalPaths&, const UpdateJournal&) = 0;
    virtual void restore_registration(const JournalPaths&, const UpdateJournal&) = 0;
    // Stop a failed health process and reacquire the app gate before touching its tree.
    virtual void stop_failed_health_process(const JournalPaths&, const UpdateJournal&) = 0;
};

using TransactionHook = std::function<void(std::string_view point)>;

// "OpenWhisper_CPP did not close, so the update was not installed. Your current version is
// unchanged — close OpenWhisper_CPP completely and try the update again." (_PARENT_STILL_RUNNING):
// the app that handed off is still there, and nothing has been moved.
std::string parent_still_running_message();
// "OpenWhisper_CPP is still running.": a recovery or rollback could not take the application gate.
std::string application_running_message();

// Input must have been parsed from a journal; each entry revalidates it against the independently
// supplied context before asking the platform to act. No active tree is overwritten. A failed
// rollback leaves the durable journal and recovery entry for a later recovery attempt.
TransactionState apply_prepared(const JournalContext&, UpdateJournal&, TransactionPlatform&,
                                const CancelToken& cancel = {}, TransactionHook hook = {});
TransactionState recover_transaction(const JournalContext&, UpdateJournal&, TransactionPlatform&);
} // namespace fv::update_native
