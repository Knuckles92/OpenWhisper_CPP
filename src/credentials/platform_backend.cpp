#include "credentials/platform_backend.h"

namespace fv {

std::string_view platform_backend_name() {
#if defined(_WIN32)
    return "Windows Credential Manager";
#elif defined(__APPLE__)
    return "macOS Keychain";
#else
    return "Secret Service (GNOME Keyring or KWallet)";
#endif
}

// make_platform_backend() is defined in win/wincred_backend.cpp and
// posix/unavailable_backend.cpp.

} // namespace fv
