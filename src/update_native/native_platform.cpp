#include "update_native/native_platform.h"
#include "update/update_contract.h"
#include "update_native/manifest.h"
#include "update_native/parent_process.h"
#include <chrono>
#include <string>
#include <utility>

namespace fv::update_native {
NativePlatform::NativePlatform(JournalContext context, std::string_view id,
                               RegistryActions& registry, update::MutexApi& mutex,
                               const update::ReleaseIdentity& identity, SetupGate setup)
    : context_(std::move(context)), actions_(context_, id), registry_(registry), mutex_(mutex),
      identity_(identity), setup_(setup) {}
void NativePlatform::set_lock_retry(LockRetry retry) {
    retry_ = retry;
    actions_.set_lock_retry(std::move(retry));
}
std::unique_ptr<TransactionGates> NativePlatform::acquire_gates() {
    GateHeld held = GateHeld::Update;
    auto result = NativeTransactionGates::acquire(mutex_, identity_, setup_, &held);
    gates_ = result.get();
    if (!result)
        throw UpdateError(std::string(transaction_refusal(held)));
    return result;
}
void NativePlatform::require_application_gate() {
    if (!gates_ || !gates_->acquire_application())
        throw UpdateError(application_running_message());
}
bool NativePlatform::parent_exited(const JournalPaths& p, const UpdateJournal& j,
                                   const CancelToken& cancel) {
    if (!wait_for_parent(j.parent_pid, p.parent_exe,
                         static_cast<std::uint64_t>(j.parent_creation_time),
                         std::chrono::seconds(15), cancel))
        return false;
    // The process is gone but its mutexes are held: another instance is running.
    return gates_ && gates_->acquire_application();
}
void NativePlatform::verify_registration(const JournalPaths&, const UpdateJournal& j) {
    require_application_gate();
    if (to_json(load_journal(context_, j.transaction_id)) != to_json(j))
        throw UpdateError("The prepared transaction has changed since this updater started.");
    registry_.verify(j);
}
TreeState NativePlatform::classify(const std::filesystem::path& path, const UpdateJournal& j,
                                   bool retain) {
    std::error_code error;
    if (!std::filesystem::exists(std::filesystem::symlink_status(path, error))) {
        if (error && error != std::errc::no_such_file_or_directory)
            throw std::filesystem::filesystem_error("Inspecting update tree", path, error);
        return TreeState::Missing;
    }
    try {
        // Pinning opens every file of the tree. A lock on one of them is waited out; a tree that
        // stays locked, or fails for any other reason, is not one of this transaction's.
        auto tree = download::retry_while_locked(
            "Update tree", [&] { return std::make_unique<PinnedTree>(path); }, retry_.timeout,
            retry_.options);
        try {
            tree->verify_release(j.new_version, j.new_exe_sha256, j.compatibility_files);
            if (retain)
                active_ = std::move(tree);
            return TreeState::New;
        } catch (const std::exception&) {
        }
        try {
            tree->verify_old(j.old_exe_sha256);
            if (retain)
                active_ = std::move(tree);
            return TreeState::Old;
        } catch (const std::exception&) {
            // The expected new executable with damaged support files still belongs to this
            // transaction. Preserve it at the failed-tree path and restore the previous install.
            tree->verify_old(j.new_exe_sha256);
            return TreeState::DamagedNew;
        }
    } catch (const std::exception&) {
        return TreeState::Foreign;
    }
}
TreeState NativePlatform::active_tree(const JournalPaths& p, const UpdateJournal& j) {
    active_.reset();
    return classify(p.install_dir, j, true);
}
TreeState NativePlatform::candidate_tree(const JournalPaths& p, const UpdateJournal& j) {
    return classify(p.candidate_dir, j, false);
}
TreeState NativePlatform::rollback_tree(const JournalPaths& p, const UpdateJournal& j) {
    return classify(p.rollback_dir, j, false);
}
void NativePlatform::snapshot_data(const JournalPaths&, const UpdateJournal&) {
    actions_.storage().acquire_data_lease();
    actions_.storage().snapshot_with_pinned_sources();
}
void NativePlatform::restore_data(const JournalPaths&, const UpdateJournal&) {
    if (!actions_.storage().has_data_lease())
        actions_.storage().acquire_data_lease();
    actions_.storage().restore_with_pinned_sources();
}
void NativePlatform::install_recovery(const JournalPaths&, const UpdateJournal& j) {
    registry_.install_recovery(j);
}
void NativePlatform::clear_recovery(const JournalPaths&, const UpdateJournal& j) {
    registry_.clear_recovery(j);
}
void NativePlatform::persist(const JournalPaths&, const UpdateJournal& j) {
    save_journal(context_, j);
}
void NativePlatform::move_old_to_rollback(const JournalPaths&) {
    active_.reset();
    actions_.move_old_to_rollback();
}
void NativePlatform::move_candidate_to_install(const JournalPaths&) {
    active_.reset();
    actions_.move_candidate_to_install();
}
void NativePlatform::move_new_to_failed(const JournalPaths&) {
    active_.reset();
    actions_.move_new_to_failed();
}
void NativePlatform::move_rollback_to_install(const JournalPaths&) {
    active_.reset();
    actions_.move_rollback_to_install();
}
bool NativePlatform::launch_and_check_health(const JournalPaths&, const UpdateJournal& j,
                                             const CancelToken& cancel) {
    if (!active_)
        throw UpdateError("The updated installation is not pinned.");
    active_->verify_release(j.new_version, j.new_exe_sha256, j.compatibility_files);
    actions_.storage().release_data_lease();
    gates_->release_application();
    return health_.launch_and_wait(actions_.storage(), j.health_token, j.new_exe_sha256,
                                   std::chrono::seconds(120), cancel);
}
bool NativePlatform::health_confirmed(const JournalPaths&, const UpdateJournal& j) {
    try {
        return actions_.storage().transaction_directory().read_file(update::kHealthName, 32) ==
               j.health_token;
    } catch (const std::exception&) {
        return false;
    }
}
void NativePlatform::update_registration(const JournalPaths&, const UpdateJournal& j) {
    registry_.update(j);
}
void NativePlatform::restore_registration(const JournalPaths&, const UpdateJournal& j) {
    registry_.restore(j);
}
void NativePlatform::stop_failed_health_process(const JournalPaths&, const UpdateJournal&) {
    health_.stop();
    require_application_gate();
}
} // namespace fv::update_native
