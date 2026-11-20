#pragma once
#include "base/json.h"
#include <optional>
#include <string_view>
namespace fv::remote {
// A new meeting must have explicit permission for the currently paired computer.
Json meeting_route(const Json& settings);
// Null for a local meeting, including a record imported into the computer that decoded it.
// A damaged route never silently changes the destination of saved audio.
std::optional<Json> saved_meeting_route(const Json& meeting, std::string_view own_fingerprint = {});
// Pin the first available meeting-capable engine and demand that same model on reconnection.
Json checked_meeting_engine(const Json& route, const Json& ready);
} // namespace fv::remote
