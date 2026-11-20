#include <pthread.h>

#include <stdexcept>

#include "focus/detail/platform.h"
#include "focus/foreground.h"
#include "focus/synthetic_keys.h"
#include "focus/uia_text_reader.h"

// The macOS and Linux readers, identities and keystrokes are not ported: identity is unknown,
// no text is read, and keystrokes are refused.
namespace fv::focus {

std::unique_ptr<Win32Desktop> make_win32_desktop() {
    throw std::runtime_error("not supported on this platform");
}

std::optional<AppIdentity> foreground_identity() {
    return std::nullopt;
}

struct UiaTextReader::Impl {};

UiaTextReader::UiaTextReader() {
    throw std::runtime_error("not supported on this platform");
}

UiaTextReader::~UiaTextReader() = default;

std::optional<TextContext> UiaTextReader::read(const AppIdentity&, bool, bool) {
    throw std::runtime_error("not supported on this platform");
}

std::optional<TextContext> UiaTextReader::read_window(std::uintptr_t, bool, bool) {
    throw std::runtime_error("not supported on this platform");
}

std::shared_ptr<KeyboardPort> make_keyboard_port() {
    throw SyntheticKeysError("not supported on this platform");
}

namespace detail {

void set_thread_name(const std::string& name) {
#if defined(__APPLE__)
    pthread_setname_np(name.substr(0, 63).c_str());
#else
    // The kernel's limit is 16 bytes including the terminator; longer names are refused.
    pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

std::shared_ptr<FocusPlatform> make_native_platform() {
    return nullptr;
}

bool native_text_supported() {
    return false;
}

} // namespace detail

} // namespace fv::focus
