#include "hotkeys/capture.h"

namespace fv::hotkeys {

// A second WH_KEYBOARD_LL hook on a thread of its own: Windows calls the hook installed last
// first, so the capture sees every key before the app's suppressing hook decides, and it always
// passes the key on.
std::unique_ptr<KeyboardHook> make_capture_hook() {
    return make_keyboard_hook();
}

} // namespace fv::hotkeys