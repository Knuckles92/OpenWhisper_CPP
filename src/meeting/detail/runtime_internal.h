#pragma once

#include <string>
#include <string_view>

#include "base/json.h"
#include "base/log.h"

// What the meeting runtime's source files share.
namespace fv::meeting::detail {

inline constexpr log::Logger runtime_logger("services.runtime.meeting");

// The terminal finalization statuses.
bool terminal_finalization(std::string_view status);

// dict(payload) without the runtime's private keys.
Json public_payload(const Json& payload);

// str(value or "") of a member; "" when `object` is not an object.
std::string text_of(const Json& object, std::string_view key);

} // namespace fv::meeting::detail
