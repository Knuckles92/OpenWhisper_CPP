#include "hotkeys/grammar.h"

#include <atomic>
#include <optional>
#include <vector>

#include "hotkeys/testing/vocabulary.h"
#include "pyport/text.h"

namespace fv::hotkeys {

namespace {

using Aliases = std::vector<std::pair<std::string_view, std::string_view>>;

// _hotkey_keyboard._MODIFIER_ALIASES; "windows" is how keyboard.get_hotkey_name spells the key.
const Aliases kWindowsModifierAliases{
    {"ctrl", "ctrl"},   {"control", "ctrl"}, {"alt", "alt"}, {"shift", "shift"}, {"win", "win"},
    {"windows", "win"}, {"super", "win"},    {"cmd", "win"}, {"meta", "win"},
};
constexpr std::array<std::string_view, 4> kWindowsOrder{"ctrl", "alt", "shift", "win"};

// _hotkey_pynput._MODIFIER_ALIASES: win/super map to cmd so Windows-style settings still name
// the Command key.
const Aliases kPynputModifierAliases{
    {"cmd", "cmd"},    {"command", "cmd"}, {"win", "cmd"},      {"super", "cmd"},
    {"meta", "cmd"},   {"ctrl", "ctrl"},   {"control", "ctrl"}, {"alt", "alt"},
    {"option", "alt"}, {"opt", "alt"},     {"shift", "shift"},
};
// _hotkey_pynput._MAIN_KEY_ALIASES: friendly names to the ones pynput reports.
const Aliases kPynputMainKeyAliases{
    {"escape", "esc"}, {"return", "enter"}, {"del", "delete"},
    {"ins", "insert"}, {"pgup", "page_up"}, {"pgdn", "page_down"},
};
constexpr std::array<std::string_view, 4> kPynputOrder{"cmd", "ctrl", "alt", "shift"};

const std::string_view* find_alias(const Aliases& aliases, std::string_view token) {
    for (const auto& [from, to] : aliases) {
        if (from == token) {
            return &to;
        }
    }
    return nullptr;
}

std::string_view mouse_display(std::string_view main_key) {
    for (const auto& [key, display] : kMouseKeys) {
        if (key == main_key) {
            return display;
        }
    }
    return {};
}

ParsedHotkey parse_with(std::string_view hotkey, const Aliases& modifier_aliases,
                        const Aliases* main_key_aliases) {
    ParsedHotkey parsed;
    if (hotkey.empty()) {
        return parsed;
    }
    std::vector<std::string> parts;
    for (const auto& part : py::split_on(hotkey, "+")) {
        if (!py::strip(part).empty()) {
            parts.push_back(py::lower(py::strip(part)));
        }
    }
    if (parts.empty()) {
        return parsed;
    }
    std::string main = parts.back();
    if (main_key_aliases != nullptr) {
        if (const auto* alias = find_alias(*main_key_aliases, main)) {
            main = std::string(*alias);
        }
    }
    parsed.main_key = std::move(main);
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (const auto* canonical = find_alias(modifier_aliases, parts[i])) {
            parsed.modifiers.emplace(*canonical);
        }
    }
    return parsed;
}

} // namespace

namespace {

// testing::ScopedVocabulary's choice, or -1 for the build's own.
std::atomic<int> forced_vocabulary{-1};

} // namespace

namespace testing {

void force_vocabulary(std::optional<Vocabulary> vocabulary) {
    forced_vocabulary.store(vocabulary ? static_cast<int>(*vocabulary) : -1);
}

} // namespace testing

Vocabulary platform_vocabulary() {
    if (const int forced = forced_vocabulary.load(); forced >= 0) {
        return static_cast<Vocabulary>(forced);
    }
#ifdef _WIN32
    return Vocabulary::Windows;
#else
    return Vocabulary::Pynput;
#endif
}

bool is_mouse_key(std::optional<std::string_view> main_key) {
    return main_key.has_value() && !mouse_display(*main_key).empty();
}

ParsedHotkey parse_hotkey(std::string_view hotkey, Vocabulary vocabulary) {
    if (vocabulary == Vocabulary::Windows) {
        return parse_with(hotkey, kWindowsModifierAliases, nullptr);
    }
    return parse_with(hotkey, kPynputModifierAliases, &kPynputMainKeyAliases);
}

std::string format_hotkey(const std::set<std::string>& modifiers,
                          const std::optional<std::string>& main_key, Vocabulary vocabulary) {
    const auto& order = vocabulary == Vocabulary::Windows ? kWindowsOrder : kPynputOrder;
    std::string out;
    auto append = [&out](std::string_view part) {
        if (!out.empty()) {
            out += '+';
        }
        out += part;
    };
    for (auto modifier : order) {
        if (modifiers.contains(std::string(modifier))) {
            append(modifier);
        }
    }
    if (main_key && !main_key->empty()) {
        append(*main_key);
    }
    return out;
}

std::string format_hotkey_display(std::string_view hotkey, Vocabulary vocabulary,
                                  bool mac_symbols) {
    if (hotkey.empty()) {
        return "";
    }
    const ParsedHotkey parsed = parse_hotkey(hotkey, vocabulary);
    if (!parsed.main_key) {
        return std::string(hotkey);
    }
    const std::string& main_key = *parsed.main_key;
    std::vector<std::string> parts;
    std::string main_display;
    if (vocabulary == Vocabulary::Windows) {
        constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kDisplay{
            {{"ctrl", "Ctrl"}, {"alt", "Alt"}, {"shift", "Shift"}, {"win", "Win"}}};
        for (const auto& [modifier, display] : kDisplay) {
            if (parsed.modifiers.contains(std::string(modifier))) {
                parts.emplace_back(display);
            }
        }
        // Numpad keys are stored as "kp *" / "kp -"; show just the symbol.
        if (main_key.starts_with("kp ")) {
            main_display = main_key.substr(3);
        } else if (is_mouse_key(main_key)) {
            main_display = mouse_display(main_key);
        } else if (py::len(main_key) == 1) {
            main_display = py::upper(main_key);
        } else {
            main_display = py::title(main_key);
        }
        parts.push_back(main_display);
    } else {
        constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kMac{
            {{"cmd", "⌘"}, {"ctrl", "⌃"}, {"alt", "⌥"}, {"shift", "⇧"}}};
        constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kLinux{
            {{"cmd", "Super"}, {"ctrl", "Ctrl"}, {"alt", "Alt"}, {"shift", "Shift"}}};
        for (const auto& [modifier, display] : mac_symbols ? kMac : kLinux) {
            if (parsed.modifiers.contains(std::string(modifier))) {
                parts.emplace_back(display);
            }
        }
        if (is_mouse_key(main_key)) {
            main_display = mouse_display(main_key);
        } else if (py::len(main_key) == 1) {
            main_display = py::upper(main_key);
        } else if (mac_symbols) {
            constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kMainKeys{{
                {"escape", "⎋"},
                {"esc", "⎋"},
                {"enter", "↩"},
                {"space", "Space"},
                {"tab", "⇥"},
                {"delete", "⌫"},
                {"backspace", "⌫"},
            }};
            main_display = py::title(main_key);
            for (const auto& [key, symbol] : kMainKeys) {
                if (key == main_key) {
                    main_display = symbol;
                }
            }
        } else {
            std::string spaced = main_key;
            for (char& c : spaced) {
                if (c == '_') {
                    c = ' ';
                }
            }
            main_display = py::title(spaced);
        }
        parts.push_back(main_display);
        if (!mac_symbols && main_display.starts_with("Kp ")) {
            parts.back() = "Num " + main_display.substr(3);
        }
    }
    const bool joined = vocabulary == Vocabulary::Pynput && mac_symbols;
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0 && !joined) {
            out += '+';
        }
        out += parts[i];
    }
    return out;
}

} // namespace fv::hotkeys
