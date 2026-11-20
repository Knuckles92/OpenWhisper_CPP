#pragma once

#include "meeting_agent/native_core.h"

namespace fv::meeting_agent {

std::unique_ptr<AgentCore> create_agent_core(std::string_view kind, CoreDeps&);

} // namespace fv::meeting_agent
