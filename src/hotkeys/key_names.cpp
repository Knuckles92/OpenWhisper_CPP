#include "hotkeys/key_names.h"

#include <stdexcept>

#include "base/json.h"
#include "hotkeys/data/keyboard_names.json.h"
#include "pyport/text.h"

namespace fv::hotkeys {

namespace {

const Json& document() {
    static const Json parsed = Json::parse(embedded::hotkeys_keyboard_names);
    return parsed;
}

KeyboardData load_data() {
    const Json& doc = document();
    KeyboardData data;
    data.layout = doc.at("layout").get<std::string>();
    for (const auto& row : doc.at("official_virtual_keys")) {
        data.official_virtual_keys[row.at(0).get<int>()] = {row.at(1).get<std::string>(),
                                                            row.at(2).get<bool>()};
    }
    for (const auto& row : doc.at("keypad_keys")) {
        data.keypad_keys.push_back(
            {row.at(0).get<int>(), row.at(1).get<int>(), row.at(2).get<int>()});
    }
    for (const auto& [name, canonical] : doc.at("canonical_names").items()) {
        data.canonical_names[name] = canonical.get<std::string>();
    }
    const Json& scan_codes = doc.at("modifier_scan_codes");
    const char* modifiers[] = {"ctrl", "alt", "shift", "win"};
    for (std::size_t i = 0; i < 4; ++i) {
        data.modifier_scan_codes[i] = scan_codes.at(modifiers[i]).get<std::vector<int>>();
    }
    data.alt_gr = {doc.at("alt_gr").at(0).get<int>(), doc.at("alt_gr").at(1).get<int>()};
    data.alt_gr_masks = doc.at("alt_gr_modifier_masks").get<std::vector<unsigned>>();
    return data;
}

KeyNameTable load_generated() {
    const Json& doc = document();
    const auto pool = doc.at("names").get<std::vector<std::string>>();
    KeyNameTable table;
    for (const auto& row : doc.at("keys")) {
        const int sc = row.at(0).get<int>();
        const int vk = row.at(1).get<int>();
        const bool extended = row.at(2).get<int>() != 0;
        const Json& names = row.at(3);
        for (unsigned mask = 0; mask < kNameStateCount; ++mask) {
            const auto index =
                names.is_array() ? names.at(mask).get<std::size_t>() : names.get<std::size_t>();
            if (index != 0) {
                table.set(sc, vk, extended, mask, pool.at(index));
            }
        }
    }
    return table;
}

} // namespace

const KeyboardData& keyboard_data() {
    static const KeyboardData data = load_data();
    return data;
}

std::string normalize_key_name(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("Can only normalize non-empty string names.");
    }
    std::string normalized = py::len(name) > 1 ? py::lower(name) : std::string(name);
    if (normalized != "_" && normalized.find('_') != std::string::npos) {
        for (char& c : normalized) {
            if (c == '_') {
                c = ' ';
            }
        }
    }
    const auto& canonical = keyboard_data().canonical_names;
    if (const auto found = canonical.find(normalized); found != canonical.end()) {
        return found->second;
    }
    return normalized;
}

bool is_keypad_key(int scan_code, int vk, bool extended) {
    for (const auto& keypad : keyboard_data().keypad_keys) {
        if (keypad[0] == scan_code && keypad[1] == vk && keypad[2] == (extended ? 1 : 0)) {
            return true;
        }
    }
    return false;
}

const KeyNameTable& KeyNameTable::generated() {
    static const KeyNameTable table = load_generated();
    return table;
}

std::uint64_t KeyNameTable::key(int scan_code, int vk, bool extended, unsigned state_mask) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(scan_code)) << 32) |
           (static_cast<std::uint64_t>(vk & 0xFFFF) << 16) |
           (static_cast<std::uint64_t>(extended ? 1 : 0) << 8) | (state_mask & 0xFF);
}

std::string_view KeyNameTable::name(int scan_code, int vk, bool extended,
                                    unsigned state_mask) const {
    const auto found = names_.find(key(scan_code, vk, extended, state_mask));
    if (found == names_.end()) {
        return {};
    }
    return pool_[found->second];
}

void KeyNameTable::set(int scan_code, int vk, bool extended, unsigned state_mask,
                       std::string_view name) {
    const std::uint64_t k = key(scan_code, vk, extended, state_mask);
    if (name.empty()) {
        names_.erase(k);
        return;
    }
    auto [entry, added] =
        pool_index_.try_emplace(std::string(name), static_cast<std::uint32_t>(pool_.size()));
    if (added) {
        pool_.emplace_back(name);
    }
    names_[k] = entry->second;
}

std::vector<std::pair<std::uint64_t, std::string>> KeyNameTable::entries() const {
    std::vector<std::pair<std::uint64_t, std::string>> rows;
    rows.reserve(names_.size());
    for (const auto& [k, index] : names_) {
        rows.emplace_back(k, pool_[index]);
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

} // namespace fv::hotkeys
