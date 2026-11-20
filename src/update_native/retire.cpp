#include "update_native/retire.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "update/update_contract.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"
#include "update_native/terminal_cleanup.h"

namespace fv::update_native {
namespace fs = std::filesystem;

void retire_transactions(const JournalContext& context, update::MutexApi& mutex,
                         const update::ReleaseIdentity& release, const RetireRecovery& recover) {
    if (!mutex.exists(release.setup_mutex()))
        throw UpdateError("The installer must hold the setup gate.");
    const auto root = context.data_root / update::kUpdatesDirname / update::kTransactionsDirname;
    if (!fs::exists(fs::symlink_status(root)))
        return;
    GuardedDirectory transactions(root);
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto id = path_to_utf8(entry.path().filename());
        if (id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos)
            ids.push_back(id);
    }
    std::ranges::sort(ids);
    for (const auto& id : ids) {
        UpdateJournal journal;
        try {
            journal = load_journal(context, id);
        } catch (const UpdateError&) {
            continue;
        }
        if (journal.state == TransactionState::Superseded ||
            journal.state == TransactionState::RolledBack ||
            journal.state == TransactionState::Healthy)
            continue;
        const auto paths = journal_paths(context, id);
        if (journal.state == TransactionState::OldMoved ||
            journal.state == TransactionState::NewActive ||
            fs::exists(fs::symlink_status(paths.rollback_dir))) {
            if (!recover)
                throw UpdateError("An interrupted update must be recovered before setup.");
            recover(journal);
        }
        // A helper accepts only Prepared. Retire what recovery did not finish before Inno
        // replaces files; a rollback it completed keeps its state, so the new tree it moved
        // aside is collected with the candidate.
        if (journal.state != TransactionState::RolledBack &&
            journal.state != TransactionState::Healthy) {
            journal.state = TransactionState::Superseded;
            save_journal(context, journal);
        }
        if (journal.state == TransactionState::Healthy)
            continue;
        try {
            (void)cleanup_terminal_candidate(context, journal);
        } catch (const std::exception&) {
        }
        try {
            remove_transaction_archive(context, journal);
        } catch (const std::exception&) {
        }
    }
}
} // namespace fv::update_native
