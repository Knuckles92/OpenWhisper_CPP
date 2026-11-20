#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Standard Webhooks signatures (https://www.standardwebhooks.com), the scheme webhooks and MCP
// Events share (services/automation/signing.py): webhook-id, webhook-timestamp and
// "webhook-signature: v1,<base64 HMAC-SHA256>" over "id.timestamp.body", keyed by the bytes
// behind a "whsec_" secret. Secrets and signatures are never logged. Every function is
// thread-safe.
namespace fv::automation::signing {

inline constexpr std::string_view PREFIX = "whsec_";
// How far a timestamp may be from now, in seconds.
inline constexpr std::int64_t DEFAULT_TOLERANCE_S = 300;

// "whsec_" and the base64 of 24 random bytes.
std::string new_secret();

// "v1," and the base64 HMAC-SHA256 of "<msg_id>.<timestamp>." followed by `body`. Throws
// py::ValueError when the secret does not start with "whsec_", is not strict base64 after it, or
// decodes to no bytes.
std::string signature(std::string_view secret, std::string_view msg_id, std::int64_t timestamp,
                      std::string_view body);

// webhook-id, webhook-timestamp and webhook-signature, in that order; `timestamp` defaults to
// now (Unix seconds). Throws as signature does.
std::vector<std::pair<std::string, std::string>>
headers(std::string_view secret, std::string_view msg_id, std::string_view body,
        std::optional<std::int64_t> timestamp = std::nullopt);

// Received headers, by name; names compare case-insensitively.
using Received = std::vector<std::pair<std::string, std::string>>;

// True when one of the space-separated "v1,<signature>" entries matches (compared in constant
// time) and the timestamp is within `tolerance_s` of `now` (default: the current time). A
// missing id, an unreadable timestamp or a bad secret is false. Throws py::TypeError for a
// signature with non-ASCII characters, as hmac.compare_digest does.
bool verify(std::string_view secret, const Received& received, std::string_view body,
            std::int64_t tolerance_s = DEFAULT_TOLERANCE_S,
            std::optional<std::int64_t> now = std::nullopt);

} // namespace fv::automation::signing
