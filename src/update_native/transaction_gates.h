#pragma once
#include <memory>
#include <string_view>
#include <vector>

#include "update/gates.h"
#include "update/release_identity.h"
#include "update_native/transaction.h"

namespace fv::update_native {
// Thread-affine: hold control gates while waiting for the parent; acquire application gates only
// after it exits. Health launch temporarily releases application gates, retaining control gates.
enum class SetupGate { Acquire, HeldByInstaller };
// Which gate refused: the setup's (also when HeldByInstaller finds no installer holding it) or
// another update's.
enum class GateHeld { Setup, Update };
// prepare_candidate's refusal (_exclusive_preparation): "Setup is already running." or "Another
// update is already in progress."
std::string_view preparation_refusal(GateHeld);
// commit_prepared_update's and recover_transaction's: "The setup program is already running."
// or "Another update is already in progress."
std::string_view transaction_refusal(GateHeld);
class NativeTransactionGates final : public TransactionGates {
public:
    // Null when a gate is held elsewhere; `held` (when given) then says which.
    static std::unique_ptr<NativeTransactionGates> acquire(update::MutexApi&,
                                                           const update::ReleaseIdentity&,
                                                           SetupGate = SetupGate::Acquire,
                                                           GateHeld* held = nullptr);
    ~NativeTransactionGates() override;
    bool acquire_application();
    void release_application();

private:
    NativeTransactionGates(update::MutexApi&, const update::ReleaseIdentity&);
    bool take(const std::vector<std::string>&, std::vector<update::MutexHandle>&);
    void release(std::vector<update::MutexHandle>&);
    update::MutexApi& api_;
    std::vector<std::string> app_names_;
    std::vector<update::MutexHandle> control_, application_;
};
} // namespace fv::update_native
