#pragma once
#include "update_native/health_process.h"
#include "update_native/pinned_tree.h"
#include "update_native/storage_actions.h"
#include "update_native/transaction.h"
#include "update_native/transaction_gates.h"
#include <memory>

namespace fv::update_native {
class RegistryActions {
public:
    virtual ~RegistryActions() = default;
    virtual void verify(const UpdateJournal&) = 0;
    virtual void install_recovery(const UpdateJournal&) = 0;
    virtual void clear_recovery(const UpdateJournal&) = 0;
    virtual void update(const UpdateJournal&) = 0;
    virtual void restore(const UpdateJournal&) = 0;
};
class NativePlatform final : public TransactionPlatform {
public:
    NativePlatform(JournalContext, std::string_view transaction_id, RegistryActions&,
                   update::MutexApi&, const update::ReleaseIdentity&,
                   SetupGate = SetupGate::Acquire);
    // Applies to the directory moves and to opening a tree for authentication: the app that
    // just closed can still hold its image for a moment (file_sha256's wait in OpenWhisper).
    void set_lock_retry(LockRetry retry);
    std::unique_ptr<TransactionGates> acquire_gates() override;
    bool parent_exited(const JournalPaths&, const UpdateJournal&, const CancelToken&) override;
    void verify_registration(const JournalPaths&, const UpdateJournal&) override;
    TreeState active_tree(const JournalPaths&, const UpdateJournal&) override;
    TreeState candidate_tree(const JournalPaths&, const UpdateJournal&) override;
    TreeState rollback_tree(const JournalPaths&, const UpdateJournal&) override;
    void snapshot_data(const JournalPaths&, const UpdateJournal&) override;
    void restore_data(const JournalPaths&, const UpdateJournal&) override;
    void install_recovery(const JournalPaths&, const UpdateJournal&) override;
    void clear_recovery(const JournalPaths&, const UpdateJournal&) override;
    void persist(const JournalPaths&, const UpdateJournal&) override;
    void move_old_to_rollback(const JournalPaths&) override;
    void move_candidate_to_install(const JournalPaths&) override;
    void move_new_to_failed(const JournalPaths&) override;
    void move_rollback_to_install(const JournalPaths&) override;
    bool launch_and_check_health(const JournalPaths&, const UpdateJournal&,
                                 const CancelToken&) override;
    bool health_confirmed(const JournalPaths&, const UpdateJournal&) override;
    void update_registration(const JournalPaths&, const UpdateJournal&) override;
    void restore_registration(const JournalPaths&, const UpdateJournal&) override;
    void stop_failed_health_process(const JournalPaths&, const UpdateJournal&) override;

private:
    TreeState classify(const std::filesystem::path&, const UpdateJournal&, bool retain);
    void require_application_gate();
    JournalContext context_;
    StorageActions actions_;
    RegistryActions& registry_;
    update::MutexApi& mutex_;
    const update::ReleaseIdentity& identity_;
    SetupGate setup_;
    LockRetry retry_;
    NativeTransactionGates* gates_ = nullptr;
    std::unique_ptr<PinnedTree> active_;
    HealthProcess health_;
};
} // namespace fv::update_native
