#include "update_native/installed_transaction.h"
#include <windows.h>

#include "base/identity.h"
#include "base/utf8.h"
#include "process/detached.h"
#include "update/apply_error.h"
#include "update/gates.h"
#include "update/update_contract.h"
#include "update_native/guarded_directory.h"
#include "update_native/manifest.h"
#include "update_native/native_platform.h"
#include "update_native/parent_process.h"
#include "update_native/retire.h"
#include "update_native/terminal_cleanup.h"
#include "update_native/transaction_runner.h"
#include "update_native/win_registry_actions.h"
#include <optional>

namespace fv::update_native {
namespace fs = std::filesystem;

bool standard_user_process() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD length{};
    const bool known =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length) != FALSE;
    CloseHandle(token);
    return known && length == sizeof(elevation) && !elevation.TokenIsElevated;
}

JournalContext registered_context(const Paths& paths) {
    const auto& release = update::release_identity();
    const auto registration = update::discover_install_registration(release);
    if (!standard_user_process() || paths.local_app_dir() != paths.data_root() || !registration ||
        registration->hive != update::RegistryHive::CurrentUser ||
        !registration->install_location.is_absolute())
        throw RequiresSetup("This installation needs the setup package to update.");
    const auto directory = registration->install_location.lexically_normal();
    return {paths.data_root(), directory,
            release.app_id,    directory / release.app_exe,
            registration,      R"(Software\Microsoft\Windows\CurrentVersion\RunOnce)"};
}

void retire_transactions_for_setup(const JournalContext& context) {
    const auto& release = update::release_identity();
    auto& mutex = update::system_mutex_api();
    retire_transactions(context, mutex, release, [&](UpdateJournal& journal) {
        WinRegistryActions registry(context, journal.transaction_id);
        NativePlatform platform(context, journal.transaction_id, registry, mutex, release,
                                SetupGate::HeldByInstaller);
        (void)recover_transaction(context, journal, platform);
    });
}

TransactionState run_installed_transaction(std::string_view transaction_id, bool recover,
                                           std::uint32_t launcher_pid,
                                           std::uint64_t launcher_time) {
    const auto id = update::validate_transaction_id(transaction_id);
    const auto context = registered_context(Paths::resolve(identity(), system_env()));
    const auto paths = journal_paths(context, id);
    GuardedDirectory directory(paths.transaction_dir);
    const auto executable = update::running_app_executable();
    if (!fs::equivalent(executable.parent_path(), paths.transaction_dir) ||
        !fs::equivalent(executable, paths.transaction_dir / updater_name))
        throw UpdateError("The updater must run from its prepared transaction.");
    if (launcher_pid && (!recover || !wait_for_parent(launcher_pid, context.parent_exe,
                                                      launcher_time, std::chrono::seconds(30))))
        throw UpdateError(application_running_message());
    auto journal = load_journal(context, id);
    RunnerActions actions;
    actions.cleanup = [&context](const UpdateJournal& finished) {
        if (finished.state == TransactionState::Healthy)
            (void)cleanup_healthy_rollback(context, finished);
        else
            (void)cleanup_terminal_candidate(context, finished);
    };
    actions.relaunch = [&context](const UpdateJournal& finished) {
        relaunch_verified(context, finished, [](const fs::path& program) {
            return start_detached(program, {}).has_value();
        });
    };
    actions.report = [&paths](std::string_view message) {
        update::write_apply_error(paths.updates_root, message);
    };
    std::optional<WinRegistryActions> registry;
    std::optional<NativePlatform> platform;
    try {
        registry.emplace(context, id);
        platform.emplace(context, id, *registry, update::system_mutex_api(),
                         update::release_identity());
    } catch (const std::exception& error) {
        update::write_apply_error(paths.updates_root, error.what());
        throw;
    }
    return run_transaction(context, journal, *platform, recover, actions);
}
} // namespace fv::update_native
