#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/input_tag.h"
#include "hotkeys/key_names.h"

namespace fv::hotkeys {

// One keyboard event as the `keyboard` package hands it to its hook callback.
struct KeyEvent {
    // `keyboard`'s scan code: the hardware one, or -vk for input that carries none (SendInput by
    // virtual key). nullopt only for events a test builds without one; the key is then known by
    // its name.
    std::optional<int> scan_code;
    int vk = 0;
    // The normalized name (held by the name table, the translator or the caller); empty when
    // `keyboard` has none.
    std::string_view name;
    bool keypad = false;
    bool down = true;
    double t = 0; // seconds on the monotonic clock, read when the hook saw the event
    // keyboard.is_pressed per modifier (Modifier bits), from the hook's own record of the keys
    // it let through before this one.
    std::uint8_t modifiers = 0;
    // The OS's own modifier state (GetAsyncKeyState), read for input with no scan code, whose
    // modifier scan codes the hook's record does not hold. nullopt where it cannot be read.
    std::optional<std::uint8_t> os_modifiers;
    // LLKHF_INJECTED: SendInput or keybd_event made the event, in this program or another.
    bool llkhf_injected = false;
    // KBDLLHOOKSTRUCT.dwExtraInfo: kSyntheticInputTag on the keys OpenWhisper_CPP sends itself.
    std::uintptr_t extra_info = 0;

    // keyboard's sign for virtual-key input: its modifiers are read from the OS.
    bool injected() const { return scan_code.value_or(0) < 0; }
    // OpenWhisper_CPP sent this key itself (a paste or a copy). The hook passes it on before any
    // callback sees it and does not record it as pressed, as `keyboard`'s direct_callback does
    // while its own send() runs (is_replaying). Another program's injected keys are not ours and
    // go through the callback like typing.
    bool own_input() const { return is_own_synthetic_input(llkhf_injected, extra_info); }
};

// What the Windows low-level hook receives (KBDLLHOOKSTRUCT and the message).
struct RawKey {
    int vk = 0;
    int scan_code = 0;
    bool extended = false;         // LLKHF_EXTENDED
    bool injected_alt = false;     // LLKHF_INJECTED and LLKHF_ALTDOWN both set
    bool down = true;              // WM_KEYDOWN or WM_SYSKEYDOWN
    bool injected = false;         // LLKHF_INJECTED
    std::uintptr_t extra_info = 0; // dwExtraInfo
};

// The lock keys' toggle state (GetKeyState & 1).
struct LockState {
    bool num_lock = false;
    bool caps_lock = false;
    bool scroll_lock = false;
};

// The bookkeeping `keyboard` does around its hook callback (_winkeyboard.prepare_intercept's
// process_key and _KeyboardListener.direct_callback): which states name the event, the event's
// name and numpad flag, and the keys it has seen pressed, which keyboard.is_pressed answers from.
// Only keys the callback let through count as pressed, as in `keyboard`.
// Affinity: owned thread "fv-hotkey-hook" (or a test); lookups in the table do not allocate.
class KeyEventTranslator {
public:
    // Names an event the table has no name for (layout_event_name in the hook); nullopt when
    // `keyboard` has none either.
    using Namer = std::function<std::optional<std::string>(int scan_code, int vk, bool extended,
                                                           unsigned state_mask)>;

    // Without a namer an event the table does not name has no name, so it matches no shortcut.
    explicit KeyEventTranslator(const KeyNameTable& names, Namer namer = {});

    // The event for the callback, or nullopt when `keyboard` passes the key on without asking:
    // VK_PACKET (SendInput of Unicode text), injected keys with Alt down, and the right Alt that
    // follows AltGr's fake left Ctrl. OpenWhisper_CPP's own keys come out as events (they still
    // move the Shift and AltGr state, as in `keyboard`), marked own_input() for the hook to pass
    // on.
    std::optional<KeyEvent> translate(const RawKey&, const LockState&, double t,
                                      std::optional<std::uint8_t> os_modifiers);
    // The callback let `event` through: record it as pressed or released.
    void passed(const KeyEvent& event);
    // keyboard.is_pressed for each modifier, as Modifier bits.
    std::uint8_t pressed_modifiers() const;
    // keyboard.is_pressed(scan_code); -vk for a virtual key.
    bool is_pressed(int scan_code) const;
    // Forgets every key (a new hook starts from nothing pressed).
    void reset();

private:
    void set_pressed(int scan_code, bool pressed);
    // `keyboard`'s lazy to_name entry: asked once per (key, state), then remembered.
    std::string_view missed_name(int scan_code, int vk, bool extended, unsigned state_mask);

    const KeyNameTable& names_;
    Namer namer_;
    // Names the namer gave ("" for none). Node-based, so a name an event points at stays put.
    std::unordered_map<std::uint64_t, std::string> missed_;
    bool shift_is_pressed_ = false;
    bool altgr_is_pressed_ = false;
    bool ignore_next_right_alt_ = false;
    // Pressed scan codes: 0..1023, and -1..-255 for virtual keys.
    std::array<bool, 1024> pressed_{};
    std::array<bool, 256> pressed_vk_{};
};

} // namespace fv::hotkeys
