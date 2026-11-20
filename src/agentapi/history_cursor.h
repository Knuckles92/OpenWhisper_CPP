#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"

// Opaque keyset cursors of the History API (services/agent_api/store.py:97-145): the URL-safe
// base64, with padding, of json.dumps({"scope": <fingerprint>, "keys": [...]}) with Python's
// default separators, where the fingerprint binds the cursor to the query's filters. The bytes
// are OpenWhisper's, so a cursor either app issued resumes in the other.
// Affinity: functions; any thread.
namespace fv::agentapi {

inline constexpr std::size_t CURSOR_MAX_CHARS = 2048;
inline constexpr std::size_t FINGERPRINT_CHARS = 24;

// _fingerprint: the first 24 hex digits of sha256(json.dumps(scope, sort_keys=True)).
std::string fingerprint(const Json& scope);

// The cursor that resumes after a row whose sort keys are `keys`.
std::string encode_cursor(std::string_view fingerprint, const Json& keys);

// _cursor_keys: the sort-key values a cursor resumes after, as a JSON array of `key_count`
// values (a finite number, then strings). Decoding is strict: base64 with '-' and '_' (or '+'
// and '/'), padding required. Throws InvalidQuery ("Invalid cursor or cursor used with different
// filters.") for anything else, or a scope outside `valid_fingerprints`.
Json cursor_keys(std::string_view cursor, const std::vector<std::string>& valid_fingerprints,
                 std::size_t key_count);

} // namespace fv::agentapi
