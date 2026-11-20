#pragma once

#include <filesystem>

// services/backup.py:586-628: a data root's transcription_history.json merged into a database.
// Blocking: never on the GUI thread.
namespace fv::backup {

// _merge_legacy_history: when `source_root` holds transcription_history.json and the database's
// history table is empty (the condition of the database's own first-run migration), its entries
// are inserted (INSERT OR REPLACE, the known columns the table has; empty text, timestamp and
// model become ""). BackupError for a file that is a link, not a file or over 8 MiB ("Legacy
// history JSON is unsafe or too large"), unreadable JSON ("Legacy history JSON is invalid"), no
// "entries" list ("Legacy history JSON has no valid entries list"), an entry that is not an
// object with a string id ("Legacy history entry is invalid"), or a database without a usable
// history table ("Cannot merge legacy history into this database schema").
void merge_legacy_history(const std::filesystem::path& db,
                          const std::filesystem::path& source_root);

} // namespace fv::backup
