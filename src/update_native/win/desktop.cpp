#include "update_native/desktop.h"

#include <windows.h>

#include <string>
#include <vector>

#include "base/env.h"
#include "base/identity.h"
#include "base/utf8.h"
#include "update/update_contract.h"
#include "update_native/installed_transaction.h"
#include "update_native/manifest.h"
#include "update_native/prepare.h"
#include "update_native/startup_recovery.h"
#include "update_native/terminal_cleanup.h"
#include "update_native/transaction_gates.h"

namespace fv::update_native {
namespace fs = std::filesystem;

update::NativeUpdateActions native_update_actions(const Paths& paths,
                                                  const update::InstallProbe& probe) {
    NativeEligibility facts;
    facts.standard_user = standard_user_process();
    facts.probe = probe;
    facts.default_data_root = paths.local_app_dir() == paths.data_root();
    std::error_code error;
    facts.registered_program =
        fs::equivalent(probe.running_dir / update::release_identity().app_exe,
                       update::running_app_executable(), error);
    facts.inventory_present =
        fs::is_regular_file(probe.running_dir / update::manifest_name(), error);
    std::vector<fs::path> program_files;
    for (const std::string_view name : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        if (const auto root = system_env().get(name); root && !root->empty())
            program_files.push_back(path_from_utf8(*root));
    }
    facts.under_program_files = under_program_files(probe.running_dir, program_files);
    if (!native_update_eligible(facts))
        return {};
    update::NativeUpdateActions actions;
    actions.prepare = [paths](const fs::path& archive, std::string_view digest,
                              std::string_view current_version, std::string_view released_version,
                              const CancelToken& cancel) {
        GateHeld held = GateHeld::Update;
        auto preparation = NativeTransactionGates::acquire(
            update::system_mutex_api(), update::release_identity(), SetupGate::Acquire, &held);
        if (!preparation)
            throw UpdateError(std::string(preparation_refusal(held)));
        const auto context = registered_context(paths);
        if (!fs::equivalent(update::running_app_executable(), context.parent_exe))
            throw UpdateError("The registered installation changed.");
        return prepare_update(context, archive, digest, current_version, released_version,
                              GetCurrentProcessId(), current_process_creation_time(), cancel);
    };
    actions.discard = [paths](const update::Handoff& handoff) {
        // A helper or a setup that is already at work owns the transaction now.
        auto gates =
            NativeTransactionGates::acquire(update::system_mutex_api(), update::release_identity());
        if (!gates)
            return;
        abandon_prepared_update(registered_context(paths), handoff);
    };
    return actions;
}

} // namespace fv::update_native
