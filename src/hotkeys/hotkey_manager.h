#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "hotkeys/debouncer.h"
#include "hotkeys/defaults.h"
#include "hotkeys/keyboard_hook.h"
#include "hotkeys/matcher.h"
#include "hotkeys/mouse_hook.h"

namespace fv::hotkeys {

// What the shortcuts call. The record and Command Mode pairs get the hook's time stamp and run
// inside the hook, so they must only hand the event on (to an OrderedDispatcher); the status
// callbacks also run there. Every other callback runs on a thread of its own per press. An unset
// callback is skipped.
struct Callbacks {
    std::function<void()> on_record_toggle;
    std::function<void(double at)> on_record_press;
    std::function<void(double at)> on_record_release;
    std::function<void()> on_cancel;
    std::function<void()> on_minimize_tray;
    std::function<void()> on_meeting_toggle;
    std::function<void(const std::string&)> on_status_update;
    std::function<void(const std::string&)> on_status_update_auto_hide;
    std::function<void(double at)> on_command_press;
    std::function<void(double at)> on_command_release;
    std::function<void()> on_scratchpad_toggle;
    std::function<void()> on_cycle_language;
    std::function<void()> on_paste_last_original;
    std::function<void()> on_route_record;
    std::function<void()> on_walkthrough;
    std::function<void()> on_screenshot;
    std::function<void()> on_paste_walkthrough;

    // The callback of a kPressActions entry, null for any other name.
    const std::function<void()>* press_action(std::string_view action) const;
    std::function<void()>* press_action(std::string_view action);
};

// The Windows hotkey backend (services/_hotkey_keyboard.py HotkeyManager): a HotkeyMatcher fed
// by a KeyboardHook and, for side-button shortcuts, a MouseHookSource. Construction installs the
// keyboard hook; when it cannot be installed the manager still keeps its state and reports
// backend_available() false with the reason. The mouse hook runs only while some shortcut uses a
// side button and capture is not suspended; its events reach the callbacks from its dispatch
// thread, as keyboard events do from the hook thread. Destroying the manager waits for a
// callback still running there, so no callback may wait on the thread that destroys it.
// Affinity: thread-safe. Its lock is held only while the matcher decides, never while a
// callback runs or a hook starts or stops.
class HotkeyManager {
public:
    // Runs one callback off the hook thread; Python starts a thread per press.
    using Spawner = std::function<void(std::function<void()>)>;
    using FamilyCallback = std::function<void(const std::string& id)>;

    // The app's manager: the platform keyboard hook and the platform side-button hook
    // (make_mouse_hook_source()), as Python's __init__ builds its MouseButtonHook. An empty map
    // means the platform defaults.
    explicit HotkeyManager(std::map<std::string, std::string> hotkeys = {});
    // A manager over the given hooks (tests pass fakes). With no `mouse_hook`, side buttons are
    // not hooked until set_mouse_hook_source(), so a test on a fake keyboard hook never installs
    // a real mouse hook.
    HotkeyManager(std::map<std::string, std::string> hotkeys, std::unique_ptr<KeyboardHook> hook,
                  MonotonicClock clock = monotonic_clock(),
                  std::unique_ptr<MouseHookSource> mouse_hook = nullptr);
    ~HotkeyManager();
    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    void set_callbacks(Callbacks callbacks);
    // Merges into the hotkeys, then removes and re-installs the hook.
    void update_hotkeys(const std::map<std::string, std::string>& hotkeys);
    // Switches the record shortcut between toggle and push-and-hold.
    void set_record_mode(TriggerMode mode);
    // Replaces one runtime family; `callback(id)` fires per press on a thread of its own. Throws
    // std::invalid_argument("Unknown shortcut family: <namespace>") for a namespace outside
    // kDynamicNamespaces. Items keep their order (a shortcut two items share goes to the first).
    void set_dynamic_hotkeys(std::string_view family, const HotkeyList& hotkeys,
                             FamilyCallback callback);
    // While a settings field captures a shortcut every event passes, holds are forgotten and the
    // mouse hook is stopped.
    void set_capture_suspended(bool suspended);
    // Re-installs the hook after sleep and resume or a lost hook, keeping hotkeys, callbacks and
    // the enabled state. Holds on side buttons carry over: their hook still reports the release.
    void rehook();
    // Removes the mouse hook, then the keyboard hook.
    void cleanup();

    // Takes the side-button hook (FakeMouseHook in tests), stopping and destroying the one it
    // replaces, and starts it when a shortcut wants it. Null leaves side buttons unhooked.
    void set_mouse_hook_source(std::unique_ptr<MouseHookSource> source);
    // _claims_mouse_button: asked inside the mouse hook for each side-button press.
    bool claims_mouse_button(MouseButton button, std::uint8_t modifiers) const;
    // _on_mouse_button: a swallowed side-button press or release, from the dispatch thread.
    void on_mouse_button(const MouseEvent& event);
    // Whether the side-button hook is running now. Not from a shortcut callback: a stop holds the
    // same lock while it waits (up to 1 s) for the dispatch thread those callbacks run on.
    bool mouse_hook_running() const;

    // Whether the record or Command Mode shortcut is held on a key right now: a rehook forgets
    // keyboard holds, so a refresh waits until this is false.
    bool keyboard_hold_active() const;
    bool keyboard_hold_active(double now) const;

    // "keyboard" while the Windows hook runs; "unavailable" with the reason otherwise.
    bool backend_available() const;
    std::string backend_name() const;
    std::string backend_error() const;

    std::map<std::string, std::string> hotkeys() const;
    std::map<std::string, std::string> dynamic_hotkeys() const;
    bool program_enabled() const;
    void set_program_enabled(bool enabled);
    // _toggle_program_enabled: flips dictation on or off and announces it.
    void toggle_program_enabled();
    bool capture_suspended() const;

    // Where the per-press callbacks run; tests run them inline.
    void set_spawner(Spawner spawner);

    // Runs `f(matcher)` under the lock (tests inspect holds and debouncers).
    template <class F> decltype(auto) with_matcher(F&& f) {
        std::lock_guard lock(mutex_);
        return f(matcher_);
    }

private:
    void setup_keyboard_hook();
    void install_hook();
    // _sync_mouse_hook: runs the mouse hook only while a side button is bound and capture is
    // live. Called without mutex_ held.
    void sync_mouse_hook();
    bool on_event(const KeyEvent& event);
    void run(const Effect& effect, const Callbacks& callbacks,
             const std::map<std::string, FamilyCallback>& families);
    void announce(bool enabled, const Callbacks& callbacks);

    mutable std::mutex mutex_;
    HotkeyMatcher matcher_;
    std::unique_ptr<KeyboardHook> hook_;
    MonotonicClock clock_;
    std::shared_ptr<const Callbacks> callbacks_ = std::make_shared<Callbacks>();
    std::shared_ptr<const std::map<std::string, FamilyCallback>> families_ =
        std::make_shared<std::map<std::string, FamilyCallback>>();
    std::shared_ptr<const Spawner> spawner_;
    bool backend_available_ = false;
    std::string backend_name_ = "unavailable";
    std::string backend_error_;
    // Serializes starting, stopping and replacing the mouse hook. Taken before mutex_, never
    // while holding it, and mutex_ is never held across a start or stop (which waits for the
    // hook's threads, and they take mutex_). Last, so the hook, whose threads call into the
    // members above, goes first.
    mutable std::mutex mouse_mutex_;
    std::unique_ptr<MouseHookSource> mouse_hook_;
};

} // namespace fv::hotkeys
