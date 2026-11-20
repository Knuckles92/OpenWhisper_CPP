#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "credentials/store.h"

namespace fv {

inline constexpr std::size_t kMaxApiKeyLength = 512;    // code points
inline constexpr std::size_t kMinMaskSuffixLength = 12; // code points before mask_key shows any
inline constexpr std::string_view kKeyringNoResponse = "the system keyring did not respond";
inline constexpr std::string_view kUnresponsiveKeyringStatus =
    "The system keyring did not respond. Saved API keys are unavailable.";

// True when the store accepted a call and never answered (its status reason says so).
bool keyring_did_not_respond(const CredentialStatus&);

// The name stripped of Unicode whitespace. Throws std::invalid_argument("Credential name is
// empty") when nothing is left.
std::string validate_credential_name(std::string_view);

// A key ready to store: surrounding Unicode whitespace dropped, then at most 512 code points,
// each in 0x21..0x7E. A smart quote or zero-width character from a rich-text paste would
// otherwise be saved and fail every request with an opaque 401. Throws std::invalid_argument
// whose what() is the user-facing copy.
std::string validate_api_key(std::string_view);

} // namespace fv
