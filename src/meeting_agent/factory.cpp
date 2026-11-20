#include "meeting_agent/factory.h"

#include <stdexcept>

#include "meeting_agent/agent_settings.h"
#include "meeting_agent/installed_core.h"

namespace fv::meeting_agent {

std::unique_ptr<AgentCore> create_agent_core(std::string_view kind, CoreDeps& deps) {
    if (kind == CORE_PI || kind == CORE_OPENCODE || kind == CORE_DIRECT) {
        return std::make_unique<NativeAgentCore>(deps);
    }
    if (is_installed_core(kind)) {
        if (!deps.reactor || !deps.launcher) {
            throw std::invalid_argument(
                "An installed meeting agent needs a reactor and process launcher.");
        }
        return std::make_unique<InstalledAgentCore>(std::string(kind), deps.settings, *deps.reactor,
                                                    deps.launcher, agents::DriverFactory{},
                                                    deps.clock ? *deps.clock : system_clock());
    }
    throw std::invalid_argument("Unsupported meeting agent core: '" + std::string(kind) + "'");
}

} // namespace fv::meeting_agent
