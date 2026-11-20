#pragma once
#include <string_view>

#include "update_native/journal.h"
namespace fv::update_native {
// Removes only the fully authenticated candidate's manifest-listed and compatibility files.
// Unknown files or changed payloads prevent removal. A candidate whose preparation was
// interrupted has no completion marker and is collected all the same. After a rollback the new
// tree that was moved aside (<install>.failed-<id>) is collected too when it is intact.
// Returns false if there was nothing to remove, true if a tree was removed completely.
bool cleanup_terminal_candidate(const JournalContext&, const UpdateJournal&);
// The previous manifest is captured during preparation. Older journals retain their rollback.
bool cleanup_healthy_rollback(const JournalContext&, const UpdateJournal&);
inline constexpr std::string_view previous_inventory = "previous-release.json";
// Removes only known, hash-checked files from a terminal transaction under the caller's gates.
bool cleanup_finished_transaction(const JournalContext&, const UpdateJournal&);
// Removes the downloaded archive a transaction was prepared from, as _remove_transaction_payloads
// does in every terminal cleanup: once the transaction is over nothing reads it again. A missing
// archive is not an error; one that cannot be removed throws and is collected by a later start
// (update::prune_stale_downloads). `archive_sha256` empty: removed without its hash being checked
// again, as the Python's unlink is.
void remove_transaction_archive(const JournalContext&, const UpdateJournal&,
                                std::string_view archive_sha256 = {});
} // namespace fv::update_native
