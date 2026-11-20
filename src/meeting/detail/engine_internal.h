#pragma once

#include <string>

#include "base/log.h"

// What the engine's source files share.
namespace fv::meeting::detail {

inline constexpr log::Logger engine_logger("meeting.engine");

// Names the calling thread (as the platform shows it in a debugger and a crash dump).
void name_engine_thread(const std::string& name);

} // namespace fv::meeting::detail
