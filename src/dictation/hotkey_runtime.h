#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "appcore/overlay_state.h"
#include "base/clock.h"
#include "base/executor.h"
#include "base/json.h"
#include "base/signal.h"
#include "base/subscription.h"
#include "dictation/automation_port.h"
#include "dictation/recording_control.h"
#include "focus/snapshot.h"
#include "focus/synthetic_keys.h"
#include "hotkeys/defaults.h"
#include "hotkeys/dispatcher.h"
#include "hotkeys/hold_machine.h"
#include "hotkeys/hotkey_manager.h"
#include "settings/store.h"
#include "text/transforms.h"

namespace fv::dictation {

// What the status line says when the Paste original shortcut's keys stayed down.
inline constexpr std::string_view KEYS_STILL_HELD =
    "Didn't paste the original: the shortcut keys were still held";
// What the status line says when the Paste walkthrough shortcut's keys stayed down.
inline constexpr std::string_view WALKTHROUGH_KEYS_STILL_HELD =
    "Walkthrough not pasted: the shortcut keys are still held";

// config.HOTKEY_WATCHDOG_INTERVAL_MS: how often the watchdog looks for a sleep and resume.
inline constexpr std::chrono::milliseconds HOTKEY_WATCHDOG_INTERVAL{10'000};
// config.HOTKEY_SLEEP_GAP_THRESHOLD_SEC: a tick this late means the computer slept.
inline constexpr std::chrono::duration<double> HOTKEY_SLEEP_GAP_THRESHOLD{30.0};
// config.HOTKEY_HOOK_REFRESH_INTERVAL_MS: the periodic re-registration of the hook.
inline constexpr std::chrono::milliseconds HOTKEY_HOOK_REFRESH_INTERVAL{5 * 60 * 1000};
// A periodic refresh is skipped this soon after the last rehook.
inline constexpr std::chrono::duration<double> HOTKEY_MIN_REHOOK_GAP{60.0};

// Where the Command Mode shortcut's press and release go (CommandRuntime). `at` is the hook's
// monotonic time in seconds. Called on the hotkey dispatcher, in hook order, as the record keys
// are.
struct CommandKeys {
    virtual ~CommandKeys() = default;
    virtual void key_pressed(double at) = 0;
    virtual void key_released(double at) = 0;
};

// Owns the hotkey configuration, the push-and-hold machine and the keyboard hook's watchdog
// (services/runtime/hotkeys.py HotkeyRuntime, Windows: no focused-window filter, Omarchy or
// macOS Accessibility intro).
//
// The record press and release run inside the hook and only enqueue to an OrderedDispatcher
// ("fv-hotkey-dispatch"), where the HoldMachine decides in hook order; what it decides is carried
// out on the UI executor, and the dispatcher waits for the answer as Python's direct call did,
// so a release never acts before its press's start has returned. The other shortcuts run on a
// thread of their own per press and post to the UI executor.
//
// Affinity: UI actor (every public method on `ui`), except record_key_pressed,
// record_key_released and the queue_* methods (any thread; the record_key_* ones run on the
// dispatcher's thread, or the caller's in tests while the dispatcher is idle) and
// paste_last_original_after_keys_up (a per-press thread: it blocks). Signals are emitted from
// the UI thread, the dispatcher or the hook; connect them on the executor that should run them.
class HotkeyRuntime {
public:
    using HotkeyMap = std::map<std::string, std::string>;

    // `manager`, `control`, `settings`, `ui`, `clock` and `keys` outlive the runtime.
    HotkeyRuntime(hotkeys::HotkeyManager& manager, RecordingControl& control,
                  SettingsStore& settings, Executor& ui, Clock& clock, focus::SyntheticKeys& keys,
                  std::function<bool()> on_wayland = {});
    // cleanup(), then waits for a press already being handled.
    ~HotkeyRuntime();
    HotkeyRuntime(const HotkeyRuntime&) = delete;
    HotkeyRuntime& operator=(const HotkeyRuntime&) = delete;

    // Connects the runtime's own signals: stt_state_changed to on_stt_state_changed. The
    // composition root connects the rest (hands_free_changed to the overlay, the requested
    // signals to their features).
    void connect_signals();
    // Merges the platform defaults under the saved hotkeys (so a newly introduced default reaches
    // existing users), applies them and the saved trigger mode to the manager, wires its
    // callbacks, announces the hotkeys (hotkeys_changed) and registers the profile and transform
    // shortcuts (refresh_profile_hotkeys).
    void setup_hotkeys();
    // Where the Command Mode shortcut goes; null (the default) drops its presses. Must outlive
    // the runtime or be reset first.
    void set_command_keys(CommandKeys* keys) { command_keys_ = keys; }
    // Registers the cleanup profile, transform and saved action shortcuts as the manager's
    // "profile", "transform" and "action" families. A shortcut that collides with any other
    // binding (a standard action, another family, another item) is skipped rather than letting
    // whichever matches first win; a disabled action registers nothing but still counts as a
    // binding. A transform library or action list that cannot be read registers none of its
    // items and keeps the others. A press emits profile_record_requested, transform_requested or
    // action_record_requested with the item's id, on the press's own thread.
    void refresh_profile_hotkeys();
    // Replaces text::load_transforms for refresh_profile_hotkeys (tests break the library).
    using TransformLoader = std::function<std::vector<text::Transform>(const Json& settings)>;
    void set_transform_loader(TransformLoader loader) { transform_loader_ = std::move(loader); }
    // The saved actions' shortcuts for refresh_profile_hotkeys (AutomationPort::action_hotkeys);
    // without one (the default) there are no actions. It may throw. Set it before
    // setup_hotkeys, or call refresh_profile_hotkeys after.
    using ActionLoader = std::function<std::vector<ActionShortcut>(const Json& settings)>;
    void set_action_hotkeys(ActionLoader loader) { action_loader_ = std::move(loader); }
    // A new record hotkey mode ("toggle" or "push_hold"; anything else is "toggle") without
    // re-hooking: drops any hold or latch and says which mode is on.
    void set_recording_trigger_mode(std::string_view mode);
    // New hotkeys from Settings, in Settings' order: applied, saved as given, announced, and
    // "Hotkeys updated".
    void update_hotkeys(const hotkeys::HotkeyList& hotkeys);
    // While a Settings field captures a shortcut every event passes and holds are forgotten.
    void set_capture_suspended(bool suspended);
    // Starts the 10 s sleep-and-resume watch and the 5 min periodic hook refresh (none on a
    // Wayland session).
    void setup_hook_watchdog();
    // Stops the push-and-hold dispatcher and the watchdog's timers.
    void cleanup();
    // A watchdog tick: a tick more than 30 s late means the computer slept, so the hook is
    // installed again; otherwise a refresh that waited for a held key runs now.
    void on_watchdog_tick();
    // The periodic refresh: skipped within 60 s of the last rehook, and deferred while the
    // record or Command Mode shortcut is held on a key (a rehook forgets holds, so the release
    // would be lost and the recording left running).
    void on_periodic_hook_refresh();
    // Re-installs the keyboard hook; a failure is logged.
    void rehook_keyboard();
    // "STT Enabled" / "STT Disabled" as overlay states.
    void on_stt_state_changed(bool enabled);
    // A status from the hook (the enable/disable shortcut): shown, and "STT Enabled" /
    // "STT Disabled" also announce the new state. Any thread.
    void update_status_with_auto_hide(const std::string& status);
    // RecordingControl::recording_state_changed: a recording that ended for any reason drops a
    // latch that belonged to it.
    void on_recording_state_changed(bool recording);

    // The hook's record press and release at `at` (seconds on the hotkeys' monotonic clock):
    // queued for the dispatcher. Any thread; never blocks.
    void queue_record_press(double at);
    void queue_record_release(double at);
    // The hook's Command Mode press and release: queued for the dispatcher, which hands them to
    // the CommandKeys. Any thread; never blocks.
    void queue_command_press(double at);
    void queue_command_release(double at);
    // Push-and-hold: the record hotkey went down / came up at `at` (now when unset). Dispatcher
    // thread.
    void record_key_pressed(std::optional<double> at = std::nullopt);
    void record_key_released(std::optional<double> at = std::nullopt);
    // The Paste original shortcut: pastes once its modifiers are up, so Ctrl+Alt+O does not
    // reach the app as Ctrl+Alt+V; "Didn't paste the original: ..." when they stay down. Runs
    // on the press's own thread, never the UI thread.
    void paste_last_original_after_keys_up();
    // The Paste walkthrough shortcut: notes the app that has focus now (focus::get_service()),
    // waits for the shortcut's modifiers to come up, then paste_walkthrough_requested(that app);
    // "Walkthrough not pasted: ..." when they stay down. Runs on the press's own thread, never
    // the UI thread.
    void paste_walkthrough_after_keys_up();

    // For tests: the dispatcher, the machine (read it only while the dispatcher is idle) and
    // whether a refresh waits for a held key.
    hotkeys::OrderedDispatcher& dispatcher() { return *dispatcher_; }
    const hotkeys::HoldMachine& hold_machine() const { return machine_; }
    bool refresh_pending() const { return refresh_pending_; }

    // The status line (controller.status_update).
    Signal<std::string> status;
    // The overlay state (controller.overlay_state_update): SttEnabled and SttDisabled.
    Signal<appcore::OverlayState> overlay_state_update;
    // Dictation was turned on or off with the enable/disable shortcut.
    Signal<bool> stt_state_changed;
    // A push-and-hold recording latched into, or left, hands-free.
    Signal<bool> hands_free_changed;
    // The hotkeys in effect (ui_controller.update_hotkey_display).
    Signal<HotkeyMap> hotkeys_changed;
    // Shortcuts whose features arrive later: the Scratchpad and language cycling (M2),
    // walkthroughs, screenshots and pasting a walkthrough (M6; the walkthrough paste carries the
    // app that had focus), cleanup profiles (M4), actions (M5) and transforms (M4), the last
    // three carrying the item's id. route_record_requested is the Route by voice shortcut
    // (controller.toggle_route_recording).
    Signal<> route_record_requested;
    Signal<> meeting_toggle_requested;
    Signal<> scratchpad_toggle_requested;
    Signal<> cycle_language_requested;
    Signal<> walkthrough_requested;
    Signal<> screenshot_requested;
    Signal<std::optional<focus::AppIdentity>> paste_walkthrough_requested;
    Signal<std::string> profile_record_requested;
    Signal<std::string> action_record_requested;
    Signal<std::string> transform_requested;

private:
    class Host;

    // Runs fn on the UI executor and waits for its answer (inline when already there); fallback
    // when the runtime stops while waiting or the UI executor drops the task.
    bool call_on_ui(const std::function<bool()>& fn, bool fallback);
    // Hands every ended recording counted since the last call to the machine; true when a latch
    // may now be stale. Dispatcher thread.
    bool sync_recordings_ended();
    bool wait_until_recording(double timeout_s);
    void latch_status();
    void arm_watchdog_timer();
    void arm_refresh_timer();
    std::chrono::steady_clock::time_point now() const { return clock_.steady(); }

    hotkeys::HotkeyManager& manager_;
    RecordingControl& control_;
    SettingsStore& settings_;
    Executor& ui_;
    Clock& clock_;
    focus::SyntheticKeys& keys_;
    std::function<bool()> on_wayland_;

    std::unique_ptr<Host> host_;
    hotkeys::HoldMachine machine_;
    // Recordings that ended (recording_state_changed(false)), counted at once on the thread
    // that announces them; the machine hears of them on the dispatcher.
    std::atomic<std::uint64_t> recordings_ended_{0};
    std::uint64_t recordings_seen_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<CommandKeys*> command_keys_{nullptr};
    TransformLoader transform_loader_; // UI thread
    ActionLoader action_loader_;       // UI thread
    // Wakes a release waiting for the stream to open.
    std::mutex stream_mutex_;
    std::condition_variable stream_changed_;
    bool stream_woken_ = false;

    // Watchdog state; UI thread.
    std::chrono::milliseconds watchdog_interval_ = HOTKEY_WATCHDOG_INTERVAL;
    std::chrono::duration<double> sleep_gap_threshold_ = HOTKEY_SLEEP_GAP_THRESHOLD;
    std::chrono::steady_clock::time_point expected_watchdog_time_;
    // Python starts from 0.0 on a clock that is never near zero: no rehook yet.
    std::optional<std::chrono::steady_clock::time_point> last_rehook_time_;
    bool refresh_pending_ = false;
    TimerHandle watchdog_timer_;
    TimerHandle refresh_timer_;

    // Lets the manager's callbacks, which outlive nothing they capture, stop at destruction.
    std::shared_ptr<CallbackGuard> guard_ = std::make_shared<CallbackGuard>();
    Subscription stt_link_;
    // Destroyed before the members above: its worker threads use the machine and the host.
    std::unique_ptr<hotkeys::OrderedDispatcher> dispatcher_;
    Subscription recording_link_;
};

} // namespace fv::dictation
