#include "ui_shell/restart_tail.h"

#include <exception>
#include <format>

#include "base/identity.h"
#include "ui_shell/bootstrap.h"

namespace fv {

int finishRun(int code, bool opensApp, const std::function<void()>& releaseGate,
              const std::filesystem::path& exe, backup::ProcessStarter& starter,
              const std::function<void(const std::string&)>& errorBox) {
    if (!opensApp || code != backup::restart_exit_code) {
        return code;
    }
    std::string reason;
    try {
        // Right before the restart: released earlier, another launch could take the mutexes
        // while this process still cleans up.
        if (releaseGate) {
            releaseGate();
        }
        const auto restarted = backup::spawn_restart(exe, starter);
        if (restarted) {
            return 0;
        }
        reason = restarted.error();
    } catch (const std::exception& error) {
        // main.py catches every Exception around the release and the spawn alike.
        reason = error.what();
    }
    const std::string product(identity().product);
    showStartupError(
        std::format("Could not restart {0}: {1} Please start {0} again manually.", product, reason),
        errorBox);
    return 1;
}

} // namespace fv
