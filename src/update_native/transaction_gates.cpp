#include "update_native/transaction_gates.h"

namespace fv::update_native {
NativeTransactionGates::NativeTransactionGates(update::MutexApi& api,
                                               const update::ReleaseIdentity& identity)
    : api_(api), app_names_(identity.app_mutexes()) {}

std::string_view preparation_refusal(GateHeld held) {
    return held == GateHeld::Setup ? "Setup is already running."
                                   : "Another update is already in progress.";
}

std::string_view transaction_refusal(GateHeld held) {
    return held == GateHeld::Setup ? "The setup program is already running."
                                   : "Another update is already in progress.";
}

std::unique_ptr<NativeTransactionGates>
NativeTransactionGates::acquire(update::MutexApi& api, const update::ReleaseIdentity& identity,
                                SetupGate setup, GateHeld* held) {
    const auto refuse = [held](GateHeld which) {
        if (held)
            *held = which;
        return std::unique_ptr<NativeTransactionGates>();
    };
    auto result =
        std::unique_ptr<NativeTransactionGates>(new NativeTransactionGates(api, identity));
    if (setup == SetupGate::HeldByInstaller && !api.exists(identity.setup_mutex()))
        return refuse(GateHeld::Setup);
    // The update mutexes, then the setup's; a refusal releases what was taken.
    if (!result->take(identity.update_mutexes(), result->control_))
        return refuse(GateHeld::Update);
    if (setup == SetupGate::Acquire) {
        std::vector<update::MutexHandle> setup_gate;
        if (!result->take({identity.setup_mutex()}, setup_gate)) {
            result->release(result->control_);
            return refuse(GateHeld::Setup);
        }
        result->control_.push_back(setup_gate.front());
    }
    return result;
}
NativeTransactionGates::~NativeTransactionGates() {
    release_application();
    release(control_);
}
bool NativeTransactionGates::take(const std::vector<std::string>& names,
                                  std::vector<update::MutexHandle>& handles) {
    for (const auto& name : names) {
        auto handle = api_.create(name);
        if (!handle) {
            release(handles);
            return false;
        }
        handles.push_back(*handle);
    }
    return true;
}
void NativeTransactionGates::release(std::vector<update::MutexHandle>& handles) {
    for (auto it = handles.rbegin(); it != handles.rend(); ++it)
        api_.release(*it);
    handles.clear();
}
bool NativeTransactionGates::acquire_application() {
    return !application_.empty() || take(app_names_, application_);
}
void NativeTransactionGates::release_application() {
    release(application_);
}
} // namespace fv::update_native
