#include "hotkeys/matcher.h"

#include <algorithm>
#include <stdexcept>

#include "hotkeys/grammar.h"
#include "pyport/text.h"

namespace fv::hotkeys {

namespace {

// Windows repeats a held key within its repeat delay (at most 1 s, 2 s with Filter Keys). A later
// key-down of the same key is a new press whose key-up the hook missed, as happens while an
// elevated window has focus.
constexpr double kRepeatWindowS = 2.5;

std::uint8_t modifier_mask(const std::set<std::string>& modifiers) {
    std::uint8_t mask = 0;
    mask |= modifiers.contains("ctrl") ? kCtrl : 0;
    mask |= modifiers.contains("alt") ? kAlt : 0;
    mask |= modifiers.contains("shift") ? kShift : 0;
    mask |= modifiers.contains("win") ? kWin : 0;
    return mask;
}

bool is_ascii(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// str.lower() into `out`, reusing its storage: ASCII (every name of a US layout) never allocates
// once the buffer has grown.
void lower_into(std::string_view text, std::string& out) {
    if (!is_ascii(text)) {
        out = py::lower(text);
        return;
    }
    out.assign(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
}

} // namespace

TriggerMode trigger_mode_from(std::string_view value) {
    return value == "push_hold" ? TriggerMode::PushHold : TriggerMode::Toggle;
}

HotkeyMatcher::HotkeyMatcher(std::map<std::string, std::string> hotkeys)
    : hotkeys_(std::move(hotkeys)), record_debouncer_(kHotkeyDebounceMs) {
    if (hotkeys_.empty()) {
        for (auto& [action, hotkey] : default_hotkeys()) {
            hotkeys_.emplace(action, hotkey);
        }
    }
    rebuild_patterns();
}

HotkeyMatcher::Pattern HotkeyMatcher::make_pattern(std::string_view hotkey) {
    Pattern pattern;
    pattern.hotkey = std::string(hotkey);
    pattern.empty = hotkey.empty();
    if (pattern.empty) {
        return pattern;
    }
    const auto parts = py::split_on(py::lower(hotkey), "+");
    const std::string& main = parts.back();
    pattern.numpad = main.starts_with("kp ");
    pattern.main_key = pattern.numpad ? main.substr(3) : main;
    const ParsedHotkey parsed = parse_hotkey(hotkey, Vocabulary::Windows);
    pattern.required = modifier_mask(parsed.modifiers);
    pattern.parsed_main = parsed.main_key;
    return pattern;
}

void HotkeyMatcher::rebuild_patterns() {
    patterns_.clear();
    for (const auto& [action, hotkey] : hotkeys_) {
        patterns_.emplace(action, make_pattern(hotkey));
    }
}

const HotkeyMatcher::Pattern& HotkeyMatcher::pattern(std::string_view action) const {
    const auto found = patterns_.find(action);
    return found == patterns_.end() ? no_pattern_ : found->second;
}

HotkeyMatcher::PhysicalKey HotkeyMatcher::physical_key(const KeyEvent& event) const {
    PhysicalKey key;
    key.keypad = event.keypad;
    if (event.scan_code) {
        key.scan_code = event.scan_code;
    } else {
        key.name = event_name_;
    }
    return key;
}

bool HotkeyMatcher::matches_main_key(const Pattern& pattern) const {
    if (pattern.empty) {
        return false;
    }
    if (!event_has_name_ || event_name_ != pattern.main_key) {
        return false;
    }
    return pattern.numpad == event_keypad_;
}

bool HotkeyMatcher::matches_hotkey(const KeyEvent& event, const Pattern& pattern) const {
    if (!matches_main_key(pattern)) {
        return false;
    }
    // keyboard represents virtual-key SendInput events as -VK; their modifier scan codes do not
    // match is_pressed's hardware map, so the OS state decides.
    if (event.injected() && event.os_modifiers) {
        return *event.os_modifiers == pattern.required;
    }
    return event.modifiers == pattern.required;
}

bool HotkeyMatcher::matches_mouse(const Pattern& pattern, std::string_view button,
                                  std::uint8_t modifiers) {
    return pattern.parsed_main && *pattern.parsed_main == button && pattern.required == modifiers;
}

bool HotkeyMatcher::is_on_key(const HoldId& hold, const Pattern& pattern,
                              const PhysicalKey& key) const {
    const auto held = hold_keys_.find(hold);
    if (held == hold_keys_.end()) {
        return matches_main_key(pattern);
    }
    return held->second == key;
}

bool HotkeyMatcher::holds(const PhysicalKey& key) const {
    if (record_key_held_ && is_on_key({"record_toggle", ""}, pattern("record_toggle"), key)) {
        return true;
    }
    if (command_key_held_ && is_on_key({"command_mode", ""}, pattern("command_mode"), key)) {
        return true;
    }
    for (const auto& [action, held] : press_held_) {
        if (is_on_key({action, ""}, held, key)) {
            return true;
        }
    }
    for (const auto& [hold, held] : dynamic_held_) {
        if (is_on_key(hold, held, key)) {
            return true;
        }
    }
    return false;
}

void HotkeyMatcher::hold_on(const HoldId& hold, const std::optional<PhysicalKey>& key) {
    if (!key) {
        hold_keys_.erase(hold);
    } else {
        hold_keys_[hold] = *key;
    }
}

void HotkeyMatcher::end_press_holds() {
    for (const auto& [action, held] : press_held_) {
        hold_keys_.erase({action, ""});
    }
    for (const auto& [hold, held] : dynamic_held_) {
        hold_keys_.erase(hold);
    }
    press_held_.clear();
    dynamic_held_.clear();
}

template <class Matches>
bool HotkeyMatcher::press(const Matches& matches, double at, const std::optional<PhysicalKey>& key,
                          Effects& effects) {
    const Pattern& enable_disable = pattern("enable_disable");
    if (matches(enable_disable)) {
        press_held_.insert_or_assign("enable_disable", enable_disable);
        hold_on({"enable_disable", ""}, key);
        effects.push(toggle_program_enabled());
        return true;
    }
    if (!program_enabled_) {
        return false;
    }
    if (matches(pattern("record_toggle"))) {
        press_record(at, key, effects);
        return true;
    }
    if (matches(pattern("command_mode"))) {
        if (!command_key_held_) {
            command_key_held_ = true;
            hold_on({"command_mode", ""}, key);
            effects.push({.kind = Effect::Kind::CommandPress, .at = at});
        }
        return true;
    }
    return press_action(matches, at, key, effects) || press_dynamic(matches, at, key, effects);
}

void HotkeyMatcher::press_record(double at, const std::optional<PhysicalKey>& key,
                                 Effects& effects) {
    if (record_key_held_) {
        return;
    }
    record_key_held_ = true;
    hold_on({"record_toggle", ""}, key);
    if (record_mode_ == TriggerMode::PushHold) {
        effects.push({.kind = Effect::Kind::RecordPress, .at = at});
    } else if (should_trigger_record_toggle(at)) {
        effects.push({.kind = Effect::Kind::RecordToggle, .at = at});
    }
}

template <class Matches>
bool HotkeyMatcher::press_action(const Matches& matches, double at,
                                 const std::optional<PhysicalKey>& key, Effects& effects) {
    for (std::string_view action : kPressActions) {
        const Pattern& candidate = pattern(action);
        if (matches(candidate)) {
            press_held_.insert_or_assign(std::string(action), candidate);
            hold_on({std::string(action), ""}, key);
            effects.push({.kind = Effect::Kind::PressAction, .at = at, .action = action});
            return true;
        }
    }
    return false;
}

template <class Matches>
bool HotkeyMatcher::press_dynamic(const Matches& matches, double at,
                                  const std::optional<PhysicalKey>& key, Effects& effects) {
    for (const auto& family : families_) {
        for (const auto& item : family.items) {
            if (!matches(item.pattern)) {
                continue;
            }
            HoldId hold{family.name, item.id};
            if (!dynamic_held_.contains(hold)) {
                dynamic_held_.emplace(hold, item.pattern);
                hold_on(hold, key);
                const auto debouncer = dynamic_debouncers_.find(hold);
                if (debouncer != dynamic_debouncers_.end() &&
                    debouncer->second.should_trigger_at(at)) {
                    effects.push({.kind = Effect::Kind::Dynamic,
                                  .at = at,
                                  .family = family.name,
                                  .id = item.id});
                }
            }
            return true;
        }
    }
    return false;
}

template <class IsOnKey>
bool HotkeyMatcher::release(const IsOnKey& is_on_key, double at, Effects& effects) {
    bool claimed = false;
    if (record_key_held_ && is_on_key(HoldId{"record_toggle", ""}, pattern("record_toggle"))) {
        record_key_held_ = false;
        hold_keys_.erase({"record_toggle", ""});
        if (record_mode_ == TriggerMode::PushHold) {
            effects.push({.kind = Effect::Kind::RecordRelease, .at = at});
        }
        claimed = true;
    }
    if (command_key_held_ && is_on_key(HoldId{"command_mode", ""}, pattern("command_mode"))) {
        command_key_held_ = false;
        hold_keys_.erase({"command_mode", ""});
        effects.push({.kind = Effect::Kind::CommandRelease, .at = at});
        claimed = true;
    }
    for (auto it = press_held_.begin(); it != press_held_.end();) {
        const HoldId hold{it->first, ""};
        if (is_on_key(hold, it->second)) {
            hold_keys_.erase(hold);
            it = press_held_.erase(it);
            claimed = true;
        } else {
            ++it;
        }
    }
    for (auto it = dynamic_held_.begin(); it != dynamic_held_.end();) {
        if (is_on_key(it->first, it->second)) {
            hold_keys_.erase(it->first);
            it = dynamic_held_.erase(it);
            claimed = true;
        } else {
            ++it;
        }
    }
    return claimed;
}

Decision HotkeyMatcher::handle(const KeyEvent& event) {
    Decision decision;
    if (capture_suspended_) {
        return decision;
    }
    const double at = event.t;
    event_has_name_ = !event.name.empty();
    lower_into(event.name, event_name_);
    event_keypad_ = event.keypad;
    const PhysicalKey key = physical_key(event);

    if (event.down) {
        const bool repeat =
            last_down_ && last_down_->first == key && at - last_down_->second <= kRepeatWindowS;
        last_down_ = std::pair{key, at};
        // Windows keeps repeating the main key after the user lets go of a modifier, so a repeat
        // can match a different shortcut (Ctrl+Num* held, Ctrl released: the repeat matches
        // Num*). A repeat of a held key is never a new press.
        if (repeat && holds(key)) {
            decision.suppress = true;
            return decision;
        }
        if (!repeat) {
            // Press-only holds exist only to absorb repeats, and a new key ends those, even when
            // a key-up was never seen.
            end_press_holds();
        }
        decision.suppress = press(
            [this, &event](const Pattern& candidate) { return matches_hotkey(event, candidate); },
            at, key, decision.effects);
        return decision;
    }
    if (last_down_ && last_down_->first == key) {
        last_down_.reset();
    }
    decision.suppress =
        release([this, &key](const HoldId& hold,
                             const Pattern& held) { return is_on_key(hold, held, key); },
                at, decision.effects);
    return decision;
}

bool HotkeyMatcher::claims_mouse_button(MouseButton button, std::uint8_t modifiers) const {
    if (capture_suspended_) {
        return false;
    }
    const std::string_view name = mouse_button_name(button);
    if (matches_mouse(pattern("enable_disable"), name, modifiers)) {
        return true;
    }
    if (!program_enabled_) {
        return false;
    }
    // _all_hotkeys: every stored action, whether a callback serves it or not, then every family
    // item.
    for (const auto& [action, candidate] : patterns_) {
        if (matches_mouse(candidate, name, modifiers)) {
            return true;
        }
    }
    for (const auto& family : families_) {
        for (const auto& item : family.items) {
            if (matches_mouse(item.pattern, name, modifiers)) {
                return true;
            }
        }
    }
    return false;
}

Decision HotkeyMatcher::handle_mouse(const MouseEvent& event) {
    Decision decision;
    const std::string_view name = mouse_button_name(event.button);
    if (event.pressed) {
        if (capture_suspended_) {
            return decision;
        }
        decision.suppress = press(
            [name, modifiers = event.modifiers](const Pattern& candidate) {
                return matches_mouse(candidate, name, modifiers);
            },
            event.at, std::nullopt, decision.effects);
        return decision;
    }
    decision.suppress = release(
        [name](const HoldId&, const Pattern& held) {
            return held.parsed_main && *held.parsed_main == name;
        },
        event.at, decision.effects);
    return decision;
}

bool HotkeyMatcher::wants_mouse_hook() const {
    if (capture_suspended_) {
        return false;
    }
    const auto bound = [](const Pattern& candidate) {
        return !candidate.empty && is_mouse_key(candidate.parsed_main);
    };
    for (const auto& [action, candidate] : patterns_) {
        if (bound(candidate)) {
            return true;
        }
    }
    for (const auto& family : families_) {
        for (const auto& item : family.items) {
            if (bound(item.pattern)) {
                return true;
            }
        }
    }
    return false;
}

bool HotkeyMatcher::keyboard_hold_active(double now) const {
    if (!last_down_ || now - last_down_->second > kRepeatWindowS) {
        return false;
    }
    const auto on_last = [this](const HoldId& hold) {
        const auto held = hold_keys_.find(hold);
        return held != hold_keys_.end() && held->second == last_down_->first;
    };
    return (record_key_held_ && on_last({"record_toggle", ""})) ||
           (command_key_held_ && on_last({"command_mode", ""}));
}

void HotkeyMatcher::update_hotkeys(const std::map<std::string, std::string>& hotkeys) {
    for (const auto& [action, hotkey] : hotkeys) {
        hotkeys_.insert_or_assign(action, hotkey);
    }
    rebuild_patterns();
}

void HotkeyMatcher::set_record_mode(TriggerMode mode) {
    record_mode_ = mode;
    record_key_held_ = false;
    hold_keys_.erase({"record_toggle", ""});
}

void HotkeyMatcher::set_dynamic_hotkeys(std::string_view family, const HotkeyList& items) {
    if (std::find(kDynamicNamespaces.begin(), kDynamicNamespaces.end(), family) ==
        kDynamicNamespaces.end()) {
        throw std::invalid_argument("Unknown shortcut family: " + std::string(family));
    }
    // dict(hotkeys): a repeated id keeps its first place and its last value.
    std::vector<DynamicItem> replaced;
    for (const auto& [id, hotkey] : items) {
        const auto same = std::find_if(replaced.begin(), replaced.end(),
                                       [&](const DynamicItem& item) { return item.id == id; });
        if (same != replaced.end()) {
            same->pattern = make_pattern(hotkey);
        } else {
            replaced.push_back({id, make_pattern(hotkey)});
        }
    }
    const auto existing = std::find_if(families_.begin(), families_.end(),
                                       [&](const Family& f) { return f.name == family; });
    if (existing != families_.end()) {
        existing->items = std::move(replaced);
    } else {
        families_.push_back({std::string(family), std::move(replaced)});
    }
    std::erase_if(dynamic_debouncers_,
                  [&](const auto& entry) { return entry.first.first == family; });
    for (const auto& [id, hotkey] : items) {
        dynamic_debouncers_.insert_or_assign(HoldId{std::string(family), id},
                                             Debouncer(kHotkeyDebounceMs));
    }
}

std::map<std::string, std::string> HotkeyMatcher::dynamic_hotkeys() const {
    std::map<std::string, std::string> all;
    for (const auto& family : families_) {
        for (const auto& item : family.items) {
            all[family.name + ":" + item.id] = item.pattern.hotkey;
        }
    }
    return all;
}

void HotkeyMatcher::set_capture_suspended(bool suspended) {
    capture_suspended_ = suspended;
    forget_held_keys();
}

void HotkeyMatcher::forget_held_keys(bool keep_mouse) {
    const auto forget = [keep_mouse](const std::string& hotkey) {
        return !(keep_mouse && is_mouse_key(parse_hotkey(hotkey, Vocabulary::Windows).main_key));
    };
    if (forget(pattern("record_toggle").hotkey)) {
        record_key_held_ = false;
    }
    if (forget(pattern("command_mode").hotkey)) {
        command_key_held_ = false;
    }
    std::erase_if(press_held_, [&](const auto& entry) { return forget(entry.second.hotkey); });
    std::erase_if(dynamic_held_, [&](const auto& entry) { return forget(entry.second.hotkey); });
    hold_keys_.clear();
    last_down_.reset();
}

Effect HotkeyMatcher::toggle_program_enabled() {
    program_enabled_ = !program_enabled_;
    record_debouncer_.reset();
    return {.kind = Effect::Kind::Status, .enabled = program_enabled_};
}

Debouncer* HotkeyMatcher::dynamic_debouncer(std::string_view family, std::string_view id) {
    const auto found = dynamic_debouncers_.find(HoldId{std::string(family), std::string(id)});
    return found == dynamic_debouncers_.end() ? nullptr : &found->second;
}

std::vector<std::string> HotkeyMatcher::press_holds() const {
    std::vector<std::string> holds;
    for (const auto& [action, held] : press_held_) {
        holds.push_back(action);
    }
    for (const auto& [hold, held] : dynamic_held_) {
        holds.push_back(hold.first + ":" + hold.second);
    }
    return holds;
}

} // namespace fv::hotkeys
