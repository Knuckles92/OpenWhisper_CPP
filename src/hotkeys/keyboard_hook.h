#pragma once

#include <functional>
#include <memory>

#include "hotkeys/debouncer.h"
#include "hotkeys/key_event.h"

namespace fv::hotkeys {

// A global keyboard hook that may swallow events: keyboard.hook(callback, suppress=True).
// Affinity: install and uninstall from one thread at a time (HotkeyManager's caller); the
// callback runs on the hook's own thread.
class KeyboardHook {
public:
    // Returns true to let the event through to the focused app. It runs inside the hook, where
    // Windows removes a hook that answers too slowly (LowLevelHooksTimeout): it decides and hands
    // work on, and never waits on a lock another thread holds for long. Keys FioriVoce sent
    // itself (KeyEvent::own_input) pass without reaching it and are not recorded as pressed.
    using Callback = std::function<bool(const KeyEvent&)>;

    virtual ~KeyboardHook() = default;
    // Throws std::runtime_error with a full sentence when the hook cannot be installed.
    virtual void install(Callback callback) = 0;
    // Removes the hook, on the thread that installed it, and waits until it is gone.
    virtual void uninstall() = 0;
    virtual bool installed() const = 0;
};

// The platform's hook. Windows: WH_KEYBOARD_LL on its own thread "fv-hotkey-hook" with a message
// loop, naming events with the layout's `keyboard` names (layout_key_names, and
// layout_event_name for a key that table misses) and stamping them with `clock`. Elsewhere
// install() throws "Global hotkeys are not supported on this platform."
std::unique_ptr<KeyboardHook> make_keyboard_hook(MonotonicClock clock = monotonic_clock());

} // namespace fv::hotkeys
