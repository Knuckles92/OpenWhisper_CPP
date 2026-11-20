#include "hotkeys/capture.h"

namespace fv::hotkeys {

// The keyboard hook stub: installing it throws, so a capture fails with the usual message.
std::unique_ptr<KeyboardHook> make_capture_hook() {
    return make_keyboard_hook();
}

} // namespace fv::hotkeys