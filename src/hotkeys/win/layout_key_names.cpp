#include "hotkeys/key_names.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "base/utf8.h"

namespace fv::hotkeys {

namespace {

std::string narrow(const wchar_t* text) {
    return path_to_utf8(std::filesystem::path(std::wstring(text)));
}

// keyboard's _winkeyboard.get_event_names, first answer only, before normalization.
std::optional<std::string> first_event_name(int sc, int vk, bool extended, unsigned mask) {
    const auto& data = keyboard_data();
    const bool keypad = is_keypad_key(sc, vk, extended);
    const auto official = data.official_virtual_keys.find(vk);
    const bool is_official = official != data.official_virtual_keys.end();
    if (keypad && is_official) {
        return official->second.first;
    }

    std::array<BYTE, 256> state{};
    state[0x10] = (mask & kNameShift) ? 0x80 : 0;
    state[0x11] = (mask & kNameAltGr) ? 0x80 : 0;
    state[0x12] = (mask & kNameAltGr) ? 0x80 : 0;
    state[0x14] = (mask & kNameCapsLock) ? 0x01 : 0;
    state[0x90] = (mask & kNameNumLock) ? 0x01 : 0;
    state[0x91] = (mask & kNameScrollLock) ? 0x01 : 0;
    std::array<wchar_t, 33> unicode{};
    const int unicode_ret = ToUnicode(static_cast<UINT>(vk), static_cast<UINT>(sc), state.data(),
                                      unicode.data(), 32, 0);
    if (unicode_ret != 0 && unicode[0] != L'\0') {
        std::string value = narrow(unicode.data());
        // ToUnicode keeps a dead key pending in the thread's state; a second call clears it,
        // as keyboard does.
        std::array<wchar_t, 33> scratch{};
        ToUnicode(static_cast<UINT>(vk), static_cast<UINT>(sc), state.data(), scratch.data(), 32,
                  0);
        return value;
    }

    // keyboard passes a c_long, which keeps the low 32 bits of the shifted scan code.
    const auto lparam = static_cast<std::uint32_t>(static_cast<std::uint32_t>(sc) << 16 |
                                                   (extended ? 1u : 0u) << 24);
    std::array<wchar_t, 1025> key_name{};
    const int name_ret = GetKeyNameTextW(static_cast<LONG>(lparam), key_name.data(), 1024);
    if (name_ret != 0 && key_name[0] != L'\0') {
        return narrow(key_name.data());
    }

    const UINT character = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0xFF;
    if (character != 0) {
        const wchar_t text[2] = {static_cast<wchar_t>(character), L'\0'};
        return narrow(text);
    }
    if (!keypad && is_official) {
        return official->second.first;
    }
    return std::nullopt;
}

bool alt_gr_named(int sc, int vk, unsigned mask) {
    const auto& data = keyboard_data();
    if (std::pair{sc, vk} != data.alt_gr) {
        return false;
    }
    return std::find(data.alt_gr_masks.begin(), data.alt_gr_masks.end(), mask) !=
           data.alt_gr_masks.end();
}

} // namespace

KeyNameTable layout_key_names() {
    const auto keys = named_keys(
        [](int code) {
            return static_cast<int>(
                MapVirtualKeyExW(static_cast<UINT>(code), MAPVK_VSC_TO_VK_EX, nullptr));
        },
        [](int vk) {
            return static_cast<int>(
                MapVirtualKeyExW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX, nullptr));
        });
    KeyNameTable table;
    for (const auto& [sc, vk, extended] : keys) {
        for (unsigned mask = 0; mask < kNameStateCount; ++mask) {
            if (const auto name = layout_event_name(sc, vk, extended != 0, mask)) {
                table.set(sc, vk, extended != 0, mask, *name);
            }
        }
    }
    return table;
}

std::optional<std::string> layout_event_name(int scan_code, int vk, bool extended,
                                             unsigned state_mask) {
    if (alt_gr_named(scan_code, vk, state_mask)) {
        return "alt gr";
    }
    if (const auto name = first_event_name(scan_code, vk, extended, state_mask)) {
        return normalize_key_name(*name);
    }
    return std::nullopt;
}

} // namespace fv::hotkeys
