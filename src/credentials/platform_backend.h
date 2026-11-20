#pragma once

#include <memory>
#include <string_view>

#include "credentials/store.h"

namespace fv {

// The name the user sees for this platform's store: "Windows Credential Manager", "macOS
// Keychain" or "Secret Service (GNOME Keyring or KWallet)".
std::string_view platform_backend_name();

// This platform's OS store, picked by platform, never by discovery, so no third-party (or
// plaintext) backend can take its place. Windows: WinCredBackend. Elsewhere, for now, a backend
// whose probe reports the store unavailable.
std::shared_ptr<CredentialBackend> make_platform_backend();

} // namespace fv
