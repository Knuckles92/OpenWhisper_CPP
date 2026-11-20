#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// The names Python's `keyboard` package gives Windows hook events, which saved shortcuts spell
// ("kp *", "page up", "!" for Shift+1 going down). tools/gen/keyboard_names.py runs the package
// itself and writes src/hotkeys/data/keyboard_names.json, embedded here.
namespace fv::hotkeys {

// The states `keyboard` names an event by, as bits of a mask, in the order its hook lists them.
enum NameState : unsigned {
    kNameShift = 1,
    kNameAltGr = 2,
    kNameNumLock = 4,
    kNameCapsLock = 8,
    kNameScrollLock = 16,
};
inline constexpr unsigned kNameStateCount = 32;

// The four modifiers a shortcut can require, as bits.
enum Modifier : std::uint8_t {
    kCtrl = 1,
    kAlt = 2,
    kShift = 4,
    kWin = 8,
};

// What `keyboard` knows about keys apart from the layout: its official virtual-key names, its
// list of numpad keys, its canonical spellings and the scan codes it checks for each modifier.
struct KeyboardData {
    std::unordered_map<int, std::pair<std::string, bool>>
        official_virtual_keys;                   // vk -> name, keypad
    std::vector<std::array<int, 3>> keypad_keys; // (scan code, virtual key, extended)
    std::unordered_map<std::string, std::string> canonical_names;
    std::array<std::vector<int>, 4> modifier_scan_codes; // ctrl, alt, shift, win
    std::pair<int, int> alt_gr{541, 162}; // the fake left Ctrl Windows sends with AltGr
    std::vector<unsigned> alt_gr_masks;   // the states `keyboard` names it "alt gr" in
    std::string layout;                   // KLID the file was generated on ("00000409")
};

// The data of the embedded file, parsed once.
const KeyboardData& keyboard_data();

// _canonical_names.normalize_name: lowercase when longer than one character, "_" as a space
// (unless the name is "_"), then the canonical spelling ("escape" -> "esc", "\r" -> "enter").
std::string normalize_key_name(std::string_view name);

bool is_keypad_key(int scan_code, int vk, bool extended);

// Event names by (scan code, virtual key, extended, state mask).
// Affinity: immutable after it is built; lookups are thread-safe and do not allocate.
class KeyNameTable {
public:
    // The names the generator recorded (on the layout keyboard_data().layout names).
    static const KeyNameTable& generated();

    // The normalized name of the event's first answer; empty when `keyboard` has none.
    std::string_view name(int scan_code, int vk, bool extended, unsigned state_mask) const;

    void set(int scan_code, int vk, bool extended, unsigned state_mask, std::string_view name);
    std::size_t size() const { return names_.size(); }

    // Every entry as (key, name), sorted, for comparing two tables.
    std::vector<std::pair<std::uint64_t, std::string>> entries() const;
    static std::uint64_t key(int scan_code, int vk, bool extended, unsigned state_mask);

private:
    std::vector<std::string> pool_{""};
    std::unordered_map<std::string, std::uint32_t> pool_index_{{"", 0}};
    std::unordered_map<std::uint64_t, std::uint32_t> names_;
};

// Every (scan code, virtual key, extended) the generator names, built from the layout's own
// scan-code mapping as tools/gen/keyboard_names.py builds it. `map_scan_to_vk(code)` and
// `map_vk_to_scan(vk)` are MapVirtualKeyExW with MAPVK_VSC_TO_VK_EX and MAPVK_VK_TO_VSC_EX.
template <class ScanToVk, class VkToScan>
std::vector<std::array<int, 3>> named_keys(ScanToVk map_scan_to_vk, VkToScan map_vk_to_scan);

// The names for the calling thread's keyboard layout, computed as `keyboard` computes them
// (ToUnicode, GetKeyNameText, MapVirtualKey, its official names). On a layout other than the
// generator's this is what `keyboard` would answer there. Off Windows, the generated table.
KeyNameTable layout_key_names();

// The normalized name `keyboard` gives one event on the calling thread's layout, computed as
// layout_key_names does; nullopt when it has none. `keyboard` names an event outside its tables
// the moment the hook sees it (the Pause key reports scan code 69 with VK_PAUSE, a pair no
// layout mapping lists), so the hook asks this on a miss. Off Windows, the generated table.
std::optional<std::string> layout_event_name(int scan_code, int vk, bool extended,
                                             unsigned state_mask);

template <class ScanToVk, class VkToScan>
std::vector<std::array<int, 3>> named_keys(ScanToVk map_scan_to_vk, VkToScan map_vk_to_scan) {
    std::vector<std::pair<int, int>> pairs;
    for (int sc = 0; sc < 0x100; ++sc) {
        pairs.emplace_back(sc, map_scan_to_vk(sc));
    }
    for (int vk = 0; vk < 0x100; ++vk) {
        pairs.emplace_back(map_vk_to_scan(vk), vk);
    }
    for (int sc = 0; sc < 0x100; ++sc) {
        pairs.emplace_back(sc, map_scan_to_vk(0xE000 | sc));
    }
    for (const auto& keypad : keyboard_data().keypad_keys) {
        pairs.emplace_back(keypad[0], keypad[1]);
    }
    for (int vk = 0; vk < 0x100; ++vk) {
        pairs.emplace_back(0, vk);
    }
    pairs.push_back(keyboard_data().alt_gr);
    std::vector<std::array<int, 3>> keys;
    for (const auto& [sc, vk] : pairs) {
        for (int extended : {0, 1}) {
            keys.push_back({sc, vk, extended});
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

} // namespace fv::hotkeys
