#pragma once

#include <cstdint>

namespace fv {

// The value FioriVoce puts in dwExtraInfo of every INPUT it hands to SendInput, keyboard and
// mouse. fv_focus's keyboard port (the paste and copy keystrokes) sets it; fv_hotkeys' low-level
// hook reads it from KBDLLHOOKSTRUCT and passes such an event on to the next hook without
// matching a shortcut on it, suppressing it, or recording its key as pressed. OpenWhisper gets
// the same from `keyboard`, which ignores events in its hook while its own send() runs; the tag
// names the events themselves, so a key the user or another program presses meanwhile still
// counts. The two libraries may not include each other, so the value lives here.
// It fits in 32 bits, the width of ULONG_PTR in a 32-bit build.
inline constexpr std::uintptr_t kSyntheticInputTag = 0x46564B53; // "FVKS"

// Whether a hook event is FioriVoce's own input: injected (LLKHF_INJECTED, LLMHF_INJECTED) and
// tagged. Input from a device is never injected, whatever its extra info holds.
constexpr bool is_own_synthetic_input(bool injected, std::uintptr_t extra_info) {
    return injected && extra_info == kSyntheticInputTag;
}

} // namespace fv
