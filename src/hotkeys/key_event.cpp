#include "hotkeys/key_event.h"

#include <utility>

namespace fv::hotkeys {

namespace {

constexpr int kVkPacket = 0xE7;
constexpr int kVkRightMenu = 0xA5;

bool is_shift_vk(int vk) {
    return vk == 0x10 || vk == 0xA0 || vk == 0xA1;
}

} // namespace

KeyEventTranslator::KeyEventTranslator(const KeyNameTable& names, Namer namer)
    : names_(names), namer_(std::move(namer)) {}

std::string_view KeyEventTranslator::missed_name(int scan_code, int vk, bool extended,
                                                 unsigned state_mask) {
    const std::uint64_t key = KeyNameTable::key(scan_code, vk, extended, state_mask);
    auto found = missed_.find(key);
    if (found == missed_.end()) {
        found =
            missed_.emplace(key, namer_(scan_code, vk, extended, state_mask).value_or("")).first;
    }
    return found->second;
}

std::optional<KeyEvent> KeyEventTranslator::translate(const RawKey& raw, const LockState& locks,
                                                      double t,
                                                      std::optional<std::uint8_t> os_modifiers) {
    if (raw.vk == kVkPacket || raw.injected_alt) {
        return std::nullopt;
    }
    // Pressing AltGr also generates an extra right Alt event.
    if (raw.vk == kVkRightMenu && ignore_next_right_alt_) {
        ignore_next_right_alt_ = false;
        return std::nullopt;
    }
    unsigned mask = 0;
    mask |= shift_is_pressed_ ? static_cast<unsigned>(kNameShift) : 0u;
    mask |= altgr_is_pressed_ ? static_cast<unsigned>(kNameAltGr) : 0u;
    mask |= locks.num_lock ? static_cast<unsigned>(kNameNumLock) : 0u;
    mask |= locks.caps_lock ? static_cast<unsigned>(kNameCapsLock) : 0u;
    mask |= locks.scroll_lock ? static_cast<unsigned>(kNameScrollLock) : 0u;

    KeyEvent event;
    event.vk = raw.vk;
    event.name = names_.name(raw.scan_code, raw.vk, raw.extended, mask);
    if (event.name.empty() && namer_) {
        event.name = missed_name(raw.scan_code, raw.vk, raw.extended, mask);
    }
    event.down = raw.down;
    event.t = t;
    event.llkhf_injected = raw.injected;
    event.extra_info = raw.extra_info;

    if (is_shift_vk(raw.vk)) {
        shift_is_pressed_ = raw.down;
    }
    const auto& alt_gr = keyboard_data().alt_gr;
    if (raw.scan_code == alt_gr.first && raw.vk == alt_gr.second) {
        ignore_next_right_alt_ = true;
        altgr_is_pressed_ = raw.down;
    }
    event.keypad = is_keypad_key(raw.scan_code, raw.vk, raw.extended);
    event.scan_code = raw.scan_code != 0 ? raw.scan_code : -raw.vk;
    event.modifiers = pressed_modifiers();
    if (event.injected()) {
        event.os_modifiers = os_modifiers;
    }
    return event;
}

void KeyEventTranslator::passed(const KeyEvent& event) {
    if (event.scan_code) {
        set_pressed(*event.scan_code, event.down);
    }
}

std::uint8_t KeyEventTranslator::pressed_modifiers() const {
    std::uint8_t modifiers = 0;
    const auto& scan_codes = keyboard_data().modifier_scan_codes;
    constexpr Modifier kBits[] = {kCtrl, kAlt, kShift, kWin};
    for (std::size_t i = 0; i < 4; ++i) {
        for (int scan_code : scan_codes[i]) {
            if (is_pressed(scan_code)) {
                modifiers |= kBits[i];
                break;
            }
        }
    }
    return modifiers;
}

void KeyEventTranslator::reset() {
    pressed_.fill(false);
    pressed_vk_.fill(false);
    shift_is_pressed_ = false;
    altgr_is_pressed_ = false;
    ignore_next_right_alt_ = false;
}

bool KeyEventTranslator::is_pressed(int scan_code) const {
    if (scan_code >= 0 && scan_code < static_cast<int>(pressed_.size())) {
        return pressed_[static_cast<std::size_t>(scan_code)];
    }
    if (scan_code < 0 && -scan_code < static_cast<int>(pressed_vk_.size())) {
        return pressed_vk_[static_cast<std::size_t>(-scan_code)];
    }
    return false;
}

void KeyEventTranslator::set_pressed(int scan_code, bool pressed) {
    // A scan code outside these ranges is no modifier, so not recording it changes nothing the
    // matcher asks.
    if (scan_code >= 0 && scan_code < static_cast<int>(pressed_.size())) {
        pressed_[static_cast<std::size_t>(scan_code)] = pressed;
    } else if (scan_code < 0 && -scan_code < static_cast<int>(pressed_vk_.size())) {
        pressed_vk_[static_cast<std::size_t>(-scan_code)] = pressed;
    }
}

} // namespace fv::hotkeys
