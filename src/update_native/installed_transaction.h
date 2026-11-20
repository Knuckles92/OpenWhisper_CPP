#pragma once
#ifdef _WIN32
#include "base/paths.h"
#include "update_native/journal.h"

namespace fv::update_native {
// The per-user transaction must never run with an elevated token. An unreadable token
// also requires the normal setup path.
bool standard_user_process();
JournalContext registered_context(const Paths&);
// Called only by the installed product's temporary setup helper while Inno holds its gate.
void retire_transactions_for_setup(const JournalContext&);
// Invoked only by the helper copied into this transaction. The registration, current-user
// data root and current executable independently determine all paths used for mutation.
TransactionState run_installed_transaction(std::string_view transaction_id, bool recover,
                                           std::uint32_t launcher_pid = 0,
                                           std::uint64_t launcher_time = 0);
} // namespace fv::update_native
#endif
