#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/json.h"

// meeting/web/auth.py: the two capability tokens of a meeting (host and guest) and the role a
// presented token grants. Comparison is constant-time, so token checking is no timing oracle;
// the host token never goes into a guest-facing payload.
// Affinity: thread-safe (functions).
namespace fv::meeting_web {

inline constexpr std::string_view kRoleHost = "host";
inline constexpr std::string_view kRoleGuest = "guest";

// Entropy per token in bytes (256 bits).
inline constexpr std::size_t kTokenBytes = 32;

// secrets.token_urlsafe(32): 43 characters of base64url without padding, from the OS CSPRNG.
std::string generate_token();
// A fresh (host_token, guest_token) pair, for a new meeting and for link rotation.
std::pair<std::string, std::string> generate_token_pair();

// hmac.compare_digest over the UTF-8 bytes; false when `expected` is empty.
bool tokens_match(std::string_view candidate, std::string_view expected);

// "host" or "guest" for a token matching the meeting's host or guest token, the host token
// checked first (equal tokens resolve to host); nullopt for an empty or absent token and for
// anything else.
std::optional<std::string> resolve_role(std::optional<std::string_view> token,
                                        std::optional<std::string_view> host_token,
                                        std::optional<std::string_view> guest_token);
// The same over a meeting row's token columns as stored: a falsy value (null, "") matches
// nothing, any other non-text value is compared as Python's str() of it.
std::optional<std::string> resolve_stored_role(std::optional<std::string_view> token,
                                               const Json& host_token, const Json& guest_token);

} // namespace fv::meeting_web
