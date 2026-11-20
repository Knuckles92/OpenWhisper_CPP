#include "hotkeys/key_names.h"

namespace fv::hotkeys {

// No Windows layout to ask: the names the generator recorded.
KeyNameTable layout_key_names() {
    return KeyNameTable::generated();
}

std::optional<std::string> layout_event_name(int scan_code, int vk, bool extended,
                                             unsigned state_mask) {
    const std::string_view name =
        KeyNameTable::generated().name(scan_code, vk, extended, state_mask);
    if (name.empty()) {
        return std::nullopt;
    }
    return std::string(name);
}

} // namespace fv::hotkeys
