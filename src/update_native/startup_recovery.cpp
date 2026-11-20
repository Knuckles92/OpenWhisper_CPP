#include "update_native/startup_recovery.h"
#include "base/hash.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "pyport/text.h"
#include "update/update_contract.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"
#include "update_native/parent_process.h"
#include "update_native/terminal_cleanup.h"
#include "update_native/transaction_gates.h"
#include <algorithm>
#include <set>

namespace fv::update_native {
namespace {
namespace fs = std::filesystem;

bool launch_verified_helper(const JournalPaths& paths, const UpdateJournal& journal,
                            const std::vector<std::string>& arguments,
                            const RecoveryLauncher& launch) {
    // A recovery launch authenticates against the original install inventory preserved on the
    // rollback tree. Only the installer previously occupying this registered path staged this
    // helper.
    const auto& original =
        fs::exists(fs::symlink_status(paths.rollback_dir)) ? paths.rollback_dir : paths.install_dir;
    GuardedDirectory rollback(original);
    const auto manifest_bytes = rollback.read_file(update::manifest_name(), max_manifest_bytes);
    if (!journal.old_manifest_sha256.empty() &&
        sha256_hex(manifest_bytes) != journal.old_manifest_sha256)
        throw UpdateError("The original recovery inventory has changed.");
    const auto doc = Json::parse(manifest_bytes);
    const auto managed = validate_manifest(doc, journal.old_version);
    const auto& app = update::release_identity().app_exe;
    auto executable = rollback.pin_file(app);
    if (!executable || managed.at(app).sha256 != journal.old_exe_sha256 ||
        download::sha256_file(original / app, true) != journal.old_exe_sha256)
        throw UpdateError("The previous installation could not be authenticated for recovery.");
    GuardedDirectory transaction(paths.transaction_dir);
    std::vector<std::unique_ptr<GuardedDirectory::FilePin>> pins;
    std::set<std::string> libraries;
    for (const auto& [name, record] : managed) {
        if (name != updater_name &&
            (name.find('/') != std::string::npos || !py::lower(name).ends_with(".dll")))
            continue;
        const auto leaf = path_from_utf8(name);
        auto pin = transaction.pin_file(leaf);
        if (!pin || fs::file_size(paths.transaction_dir / leaf) != record.size ||
            download::sha256_file(paths.transaction_dir / leaf, true) != record.sha256)
            throw UpdateError("The staged recovery helper has changed. Use the setup package.");
        libraries.insert(py::lower(name));
        pins.push_back(std::move(pin));
    }
    for (const auto& entry : fs::directory_iterator(paths.transaction_dir)) {
        const auto leaf = py::lower(path_to_utf8(entry.path().filename()));
        if (leaf.ends_with(".dll") && !libraries.contains(leaf))
            throw UpdateError("The recovery folder contains an unexpected library.");
    }
    if (!launch || !launch(paths.transaction_dir / updater_name, arguments))
        throw UpdateError("The update recovery helper could not be started.");
    return true;
}
} // namespace

bool recover_before_start(const JournalContext& context, update::MutexApi& mutex, std::uint32_t pid,
                          std::int64_t created, const RecoveryLauncher& launch,
                          const std::function<bool(const UpdateJournal&)>& retire) {
    const auto root = context.data_root / update::kUpdatesDirname / update::kTransactionsDirname;
    if (!fs::exists(fs::symlink_status(root)))
        return false;
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
        const auto paths = journal_paths(context, id);
        if (journal.state == TransactionState::Healthy ||
            journal.state == TransactionState::RolledBack ||
            journal.state == TransactionState::Superseded) {
            auto gates = NativeTransactionGates::acquire(mutex, update::release_identity());
            if (gates && gates->acquire_application()) {
                try {
                    if (!retire || !retire(journal))
                        continue;
                    if (journal.state == TransactionState::Healthy)
                        (void)cleanup_healthy_rollback(context, journal);
                    else
                        (void)cleanup_terminal_candidate(context, journal);
                    try {
                        remove_transaction_archive(context, journal);
                    } catch (const std::exception&) {
                        // Collected by prune_stale_downloads at a later start.
                    }
                    (void)cleanup_finished_transaction(context, journal);
                } catch (const std::exception&) {
                    // A held helper or an unknown entry leaves the transaction for later.
                }
            }
            continue;
        }
        if (journal.state == TransactionState::Preparing ||
            journal.state == TransactionState::Prepared) {
            if (!fs::exists(fs::symlink_status(paths.rollback_dir))) {
                try {
                    if (journal.parent_pid &&
                        !wait_for_parent(journal.parent_pid, context.parent_exe,
                                         static_cast<std::uint64_t>(journal.parent_creation_time),
                                         std::chrono::milliseconds(0)))
                        continue;
                } catch (const std::exception&) {
                    // The identity is inaccessible or the PID was reused. Leave the preparation
                    // intact.
                    continue;
                }
                auto gates = NativeTransactionGates::acquire(mutex, update::release_identity());
                if (!gates)
                    continue;
                // The journal may have advanced while we waited for control.
                auto fresh = load_journal(context, id);
                if (fresh.state != journal.state ||
                    fs::exists(fs::symlink_status(paths.rollback_dir)))
                    continue;
                fresh.state = TransactionState::Superseded;
                save_journal(context, fresh);
                // The candidate is collected when it verifies as this release's files, with or
                // without its completion marker; anything else is preserved.
                try {
                    (void)cleanup_terminal_candidate(context, fresh);
                } catch (const std::exception&) {
                }
                try {
                    remove_transaction_archive(context, fresh);
                } catch (const std::exception&) {
                }
                continue;
            }
            if (journal.state == TransactionState::Preparing)
                throw UpdateError("An unstarted update has an unexpected rollback directory.");
        } else if (journal.state != TransactionState::OldMoved &&
                   journal.state != TransactionState::NewActive) {
            continue;
        }
        return launch_verified_helper(paths, journal,
                                      {"--recover", std::string(update::kTransactionArg), id,
                                       "--wait-for-launcher", std::to_string(pid),
                                       std::to_string(created)},
                                      launch);
    }
    return false;
}
} // namespace fv::update_native
