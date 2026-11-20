#include <filesystem>
#include <string>
#include <vector>

#include "base/env.h"
#include "base/utf8.h"
#include "update_native/updater_cli.h"
#include "update_native/updater_log.h"
#ifdef _WIN32
#include "base/identity.h"
#include "base/paths.h"
#include "update/download.h"
#include "update/gates.h"
#include "update/install_probe.h"
#include "update_native/installed_transaction.h"
#include "update_native/manifest.h"
#include "update_native/transaction_runner.h"
#include <windows.h>
#endif

// openwhisper-cpp-updater: what the command line means is update_native/updater_cli.h. This file
// only connects it to the installed product: the registry, the named mutexes and the real
// processes.
namespace {

#ifdef _WIN32
bool registered_directory(const std::filesystem::path& expected,
                          const std::filesystem::path& install) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (fs::equivalent(expected, install, error))
        return true;
    // An interrupted directory swap can leave the registered leaf absent. Its existing
    // parent must still name the same physical directory.
    return expected.is_absolute() && install.is_absolute() &&
           !fs::exists(fs::symlink_status(install)) && !fs::exists(fs::symlink_status(expected)) &&
           fs::equivalent(expected.parent_path(), install.parent_path(), error) &&
           CompareStringOrdinal(expected.filename().c_str(), -1, install.filename().c_str(), -1,
                                TRUE) == CSTR_EQUAL;
}

int setup_action(std::string_view action, const std::filesystem::path& install) {
    namespace u = fv::update_native;
    const auto& identity = fv::update::release_identity();
    if (!u::standard_user_process() ||
        !fv::update::system_mutex_api().exists(identity.setup_mutex()))
        throw u::UpdateError(
            "The installer must start this operation in the current user session.");
    u::SetupSession session;
    session.helper_dir = fv::update::running_app_dir();
    session.registered = [&identity](const std::filesystem::path& target) {
        const auto registration = fv::update::discover_install_registration(identity);
        return registration && registration->hive == fv::update::RegistryHive::CurrentUser &&
               registered_directory(registration->install_location.lexically_normal(), target);
    };
    session.retire_transactions = [] {
        const auto paths = fv::Paths::resolve(fv::identity(), fv::system_env());
        if (paths.data_root() == paths.local_app_dir())
            u::retire_transactions_for_setup(u::registered_context(paths));
    };
    return u::run_setup_action(action, install, session);
}
#endif

fv::update_native::UpdaterActions installed_actions() {
    fv::update_native::UpdaterActions actions;
    actions.log_file = fv::update_native::updater_log_path(fv::system_env());
#ifdef _WIN32
    actions.setup_action = setup_action;
    actions.run_transaction = [](std::string_view id, bool recover, std::uint32_t launcher_pid,
                                 std::uint64_t launcher_time) {
        return fv::update_native::run_installed_transaction(id, recover, launcher_pid,
                                                            launcher_time);
    };
    actions.leave_launch_directory = [] {
        const auto paths = fv::Paths::resolve(fv::identity(), fv::system_env());
        (void)fv::update_native::leave_launch_directory(fv::update::updates_root(paths));
    };
    actions.message_box = [](std::string_view message) {
        fv::update::native_message_box(message);
    };
#endif
    return actions;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(fv::path_to_utf8(std::filesystem::path(argv[i])));
    }
    return fv::update_native::run_updater(args, installed_actions());
}
#else
int main(int argc, char** argv) {
    return fv::update_native::run_updater(std::vector<std::string>(argv + 1, argv + argc),
                                          installed_actions());
}
#endif
