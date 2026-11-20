#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hotkeys/debouncer.h"
#include "hotkeys/defaults.h"
#include "hotkeys/key_event.h"
#include "hotkeys/mouse_hook.h"

namespace fv::hotkeys {

// RecordingTriggerMode: "toggle" (a press starts, the next stops) or "push_hold" (record while
// the key is down).
enum class TriggerMode { Toggle, PushHold };

// The stored value: "push_hold" is push-and-hold, anything else toggles (Python compares with
// PUSH_HOLD only).
TriggerMode trigger_mode_from(std::string_view value);

// What a matched event asks the manager to run.
struct Effect {
    enum class Kind {
        RecordToggle,   // on_record_toggle, on a thread of its own
        RecordPress,    // on_record_press(at), inside the hook
        RecordRelease,  // on_record_release(at), inside the hook
        CommandPress,   // on_command_press(at), inside the hook
        CommandRelease, // on_command_release(at), inside the hook
        PressAction,    // the press action's callback, on a thread of its own
        Dynamic,        // the family's callback(id), on a thread of its own
        Status,         // "STT Enabled" / "STT Disabled", inside the hook
    };
    Kind kind = Kind::RecordToggle;
    double at = 0;           // the event's time stamp
    std::string_view action; // PressAction: an entry of kPressActions
    std::string family;      // Dynamic: the namespace
    std::string id;          // Dynamic: the item
    bool enabled = false;    // Status: dictation is now enabled
};

// At most two effects come from one event (a release ending a record and a Command Mode hold).
// Unused slots hold nothing, so the decision on a key no shortcut claims, most typing, builds no
// string (a debug build allocates even for an empty std::string).
class Effects {
public:
    class Iterator {
    public:
        explicit Iterator(const std::optional<Effect>* at) : at_(at) {}
        const Effect& operator*() const { return **at_; }
        const Effect* operator->() const { return &**at_; }
        Iterator& operator++() {
            ++at_;
            return *this;
        }
        friend bool operator==(const Iterator&, const Iterator&) = default;

    private:
        const std::optional<Effect>* at_;
    };

    void push(Effect effect) { items_.at(size_++).emplace(std::move(effect)); }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    Iterator begin() const { return Iterator(items_.data()); }
    Iterator end() const { return Iterator(items_.data() + size_); }
    const Effect& operator[](std::size_t i) const { return *items_.at(i); }

private:
    std::array<std::optional<Effect>, 4> items_{};
    std::size_t size_ = 0;
};

struct Decision {
    bool suppress = false; // the focused app never sees the event
    Effects effects;
};

// The Windows backend's decisions (services/_hotkey_keyboard.py HotkeyManager): which hook
// events a shortcut claims, and the callbacks they fire.
//
// A key is a physical key, (scan code, numpad): `keyboard` names an event from the Shift state of
// the moment, so Ctrl+Shift+1 goes down as "!" and may come up as "1". A key-down of the key that
// went down last, within 2.5 s, is an auto-repeat; a repeat of a held key is swallowed whatever
// the modifiers do now, and a new key ends every press-only hold, which recovers key-ups an
// elevated window kept from the hook. A press tries enable/disable (even while disabled), then
// the disabled gate, record, Command Mode, the press actions in table order, and the families in
// registration order; the first match claims it. A release ends every hold on its key, ignores
// modifiers and skips the enabled gate, so disabling mid-hold never strands a recording. A side
// button (mouse4, mouse5) is a main key like any other; the mouse hook delivers it.
// Affinity: not thread-safe; HotkeyManager serializes it.
class HotkeyMatcher {
public:
    // An empty map means the platform defaults, as `hotkeys or DEFAULT_HOTKEYS.copy()`.
    explicit HotkeyMatcher(std::map<std::string, std::string> hotkeys = {});

    // _handle_keyboard_event.
    Decision handle(const KeyEvent& event);

    // _claims_mouse_button, asked inside the mouse hook: whether a side-button press with these
    // modifiers is some shortcut (its exact modifier set and button). Nothing is claimed while
    // capture is suspended; enable/disable is claimed even while disabled, and nothing else is.
    // Does not allocate.
    bool claims_mouse_button(MouseButton button, std::uint8_t modifiers) const;
    // _on_mouse_button: a swallowed side-button press dispatches like a key press (no repeat
    // window, no physical key: a mouse hold is known by its button), ignored while capture is
    // suspended; a release ends every hold whose shortcut ends in that button, whatever the
    // modifiers and the enabled state. `suppress` says whether the event belonged to a shortcut.
    Decision handle_mouse(const MouseEvent& event);
    // _sync_mouse_hook's wish: some shortcut uses a side button and capture is not suspended.
    bool wants_mouse_hook() const;

    // keyboard_hold_active: the record or Command Mode shortcut is held on the key that went down
    // last, and that key went down (or repeated) at most 2.5 s before `now`.
    bool keyboard_hold_active(double now) const;

    const std::map<std::string, std::string>& hotkeys() const { return hotkeys_; }
    // Merges `hotkeys` over the current ones (dict.update).
    void update_hotkeys(const std::map<std::string, std::string>& hotkeys);

    // Resets the record hold.
    void set_record_mode(TriggerMode mode);
    TriggerMode record_mode() const { return record_mode_; }

    // Replaces one family; each item gets a fresh debouncer. Throws std::invalid_argument
    // "Unknown shortcut family: <namespace>" for a namespace outside kDynamicNamespaces.
    void set_dynamic_hotkeys(std::string_view family, const HotkeyList& items);
    // Every family's items as "<namespace>:<id>" -> hotkey (_all_hotkeys' family part).
    std::map<std::string, std::string> dynamic_hotkeys() const;

    // While a settings field captures a shortcut nothing is claimed; holds are forgotten.
    void set_capture_suspended(bool suspended);
    bool capture_suspended() const { return capture_suspended_; }

    // A rehook may miss a key-up while unhooked, so it forgets the keyboard holds; with
    // keep_mouse the holds on side buttons stay (their hook still sees the release).
    void forget_held_keys(bool keep_mouse = false);

    bool program_enabled() const { return program_enabled_; }
    void set_program_enabled(bool enabled) { program_enabled_ = enabled; }
    // _toggle_program_enabled: flips the flag, clears the record debouncer, and returns the
    // status to announce.
    Effect toggle_program_enabled();

    // _should_trigger_record_toggle at `now`.
    bool should_trigger_record_toggle(double now) {
        return record_debouncer_.should_trigger_at(now);
    }
    Debouncer& record_debouncer() { return record_debouncer_; }
    Debouncer* dynamic_debouncer(std::string_view family, std::string_view id);

    bool record_key_held() const { return record_key_held_; }
    bool command_key_held() const { return command_key_held_; }
    // Holds with no key recorded, as a missed key-up or a rehook can leave them (tests).
    void set_record_key_held(bool held) { record_key_held_ = held; }
    void set_command_key_held(bool held) { command_key_held_ = held; }
    // The press-only holds: press actions and enable/disable, then family items as "ns:id".
    std::vector<std::string> press_holds() const;

private:
    struct PhysicalKey {
        std::optional<int> scan_code;
        // Lowercased; only when there is no scan code, so a hook event builds no string.
        std::optional<std::string> name;
        bool keypad = false;
        friend bool operator==(const PhysicalKey&, const PhysicalKey&) = default;
    };
    // A shortcut string ready for matching.
    struct Pattern {
        std::string hotkey;
        bool empty = true;
        std::string main_key; // hotkey.lower().split("+")[-1], without "kp "
        bool numpad = false;
        std::uint8_t required = 0;
        std::optional<std::string> parsed_main; // parse_hotkey's main key
    };
    // ("record_toggle", "") for standard holds, (namespace, id) for family items.
    using HoldId = std::pair<std::string, std::string>;
    struct DynamicItem {
        std::string id;
        Pattern pattern;
    };
    struct Family {
        std::string name;
        std::vector<DynamicItem> items;
    };

    static Pattern make_pattern(std::string_view hotkey);
    const Pattern& pattern(std::string_view action) const;
    void rebuild_patterns();

    bool matches_main_key(const Pattern& pattern) const;
    bool matches_hotkey(const KeyEvent& event, const Pattern& pattern) const;
    // parse_hotkey(hotkey) == (modifiers, button).
    static bool matches_mouse(const Pattern& pattern, std::string_view button,
                              std::uint8_t modifiers);
    bool is_on_key(const HoldId& hold, const Pattern& pattern, const PhysicalKey& key) const;
    bool holds(const PhysicalKey& key) const;
    void hold_on(const HoldId& hold, const std::optional<PhysicalKey>& key);
    void end_press_holds();
    // `matches(pattern)` says whether the pressed key or button spells a shortcut; `key` is the
    // physical key of a keyboard press, nullopt for a side button. Each is defined, and used
    // only, in matcher.cpp.
    template <class Matches>
    bool press(const Matches& matches, double at, const std::optional<PhysicalKey>& key,
               Effects& effects);
    void press_record(double at, const std::optional<PhysicalKey>& key, Effects& effects);
    template <class Matches>
    bool press_action(const Matches& matches, double at, const std::optional<PhysicalKey>& key,
                      Effects& effects);
    template <class Matches>
    bool press_dynamic(const Matches& matches, double at, const std::optional<PhysicalKey>& key,
                       Effects& effects);
    // `is_on_key(hold, pattern)` says whether the released key or button is the one a hold was
    // pressed on.
    template <class IsOnKey> bool release(const IsOnKey& is_on_key, double at, Effects& effects);
    PhysicalKey physical_key(const KeyEvent& event) const;

    std::map<std::string, std::string> hotkeys_;
    std::map<std::string, Pattern, std::less<>> patterns_;
    Pattern no_pattern_;
    bool program_enabled_ = true;
    TriggerMode record_mode_ = TriggerMode::Toggle;
    Debouncer record_debouncer_;
    bool record_key_held_ = false;
    bool command_key_held_ = false;
    std::map<std::string, Pattern, std::less<>> press_held_; // action -> its hotkey
    std::vector<Family> families_;                           // registration order
    std::map<HoldId, Debouncer> dynamic_debouncers_;
    std::map<HoldId, Pattern> dynamic_held_; // item -> its hotkey
    std::map<HoldId, PhysicalKey> hold_keys_;
    std::optional<std::pair<PhysicalKey, double>> last_down_;
    bool capture_suspended_ = false;
    // The event being handled, lowercased once (keyboard's event.name.lower()).
    std::string event_name_;
    bool event_has_name_ = false;
    bool event_keypad_ = false;
};

} // namespace fv::hotkeys
