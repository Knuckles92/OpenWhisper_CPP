#pragma once

#include <functional>

#include "update/gates.h"
#include "update/release_identity.h"
#include "update_native/journal.h"

namespace fv::update_native {
// Puts an interrupted transaction's trees back: recover_transaction on a platform that knows the
// installer already holds the setup gate.
using RetireRecovery = std::function<void(UpdateJournal&)>;

// Retires this installation's native transactions before a setup replaces its files
// (services/setup_update.py: retire_native_transactions). The installer must hold the setup
// gate, which `mutex` has to show. A transaction that had started moving trees is finished
// first through `recover` (rolled back, or confirmed when the new version had acknowledged);
// every other unfinished one becomes superseded, so a helper left over from it can no longer
// move anything. The verified candidate, and a new tree a rollback moved aside, are collected.
// Finished and unreadable transactions are left as they are. Throws UpdateError.
void retire_transactions(const JournalContext&, update::MutexApi& mutex,
                         const update::ReleaseIdentity&, const RetireRecovery& recover);
} // namespace fv::update_native
