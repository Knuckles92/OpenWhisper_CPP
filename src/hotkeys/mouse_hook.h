#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include "hotkeys/debouncer.h"
#include "hotkeys/key_names.h"

// Side mouse buttons as shortcut keys (services/_mouse_hook_win.py): a low-level mouse hook that
// swallows the side-button presses a shortcut claims, and their releases, and hands them to a
// dispatch thread. A source made with a ClickObserver also watches left and right clicks for a
// walkthrough's Auto Mode, passing every one of them on.
namespace fv::hotkeys {

enum class MouseButton { Mouse4, Mouse5 };

// "mouse4" or "mouse5": the main key a shortcut spells the button with.
std::string_view mouse_button_name(MouseButton button);
// The inverse; nullopt for any other name.
std::optional<MouseButton> mouse_button_from_name(std::string_view name);
// HIWORD(mouseData) of an X button message: XBUTTON1 is Mouse 4, XBUTTON2 Mouse 5.
std::optional<MouseButton> mouse_button_from_xbutton(unsigned xbutton);

// One side-button event as the dispatch thread hands it on.
struct MouseEvent {
    MouseButton button = MouseButton::Mouse4;
    bool pressed = true;
    // Modifier bits held at the press (GetAsyncKeyState); a release carries none, since it is
    // matched by its button alone.
    std::uint8_t modifiers = 0;
    double at = 0; // seconds on the monotonic clock, read inside the hook

    friend bool operator==(const MouseEvent&, const MouseEvent&) = default;
};

// The Win32 values the hook's decision reads. They are plain numbers so the decision builds and is
// tested on every platform.
inline constexpr unsigned kWmXButtonDown = 0x020B;
inline constexpr unsigned kWmXButtonUp = 0x020C;
inline constexpr unsigned kWmLButtonDown = 0x0201;
inline constexpr unsigned kWmLButtonUp = 0x0202;
inline constexpr unsigned kWmRButtonDown = 0x0204;
inline constexpr unsigned kWmRButtonUp = 0x0205;
inline constexpr std::uint32_t kLlmhfInjected = 0x01;
inline constexpr int kVkShift = 0x10;
inline constexpr int kVkControl = 0x11;
inline constexpr int kVkMenu = 0x12;
inline constexpr int kVkLWin = 0x5B;
inline constexpr int kVkRWin = 0x5C;

// _held_modifiers: Modifier bits of the keys `key_state(vk)` (GetAsyncKeyState) reports down.
template <class KeyState> std::uint8_t held_modifiers(KeyState&& key_state) {
    const auto down = [&key_state](int vk) {
        return (static_cast<unsigned>(static_cast<std::uint16_t>(key_state(vk))) & 0x8000U) != 0;
    };
    std::uint8_t mask = 0;
    mask |= down(kVkControl) ? kCtrl : 0;
    mask |= down(kVkMenu) ? kAlt : 0;
    mask |= down(kVkShift) ? kShift : 0;
    mask |= (down(kVkLWin) || down(kVkRWin)) ? kWin : 0;
    return mask;
}

// Where side-button events come from: MouseButtonHook's start/stop surface.
// Affinity: start, stop and running from one thread at a time (HotkeyManager's caller); `claims`
// runs inside the hook, `on_button` on the source's dispatch thread.
class MouseHookSource {
public:
    // Asked inside the hook for each real side-button press: true swallows it (and later its
    // release), so the focused app never sees Back or Forward. It must answer at once.
    using Claims = std::function<bool(MouseButton, std::uint8_t modifiers)>;
    // A swallowed press or release, called on the dispatch thread, never inside the hook.
    using OnButton = std::function<void(const MouseEvent&)>;

    virtual ~MouseHookSource() = default;
    // Installs the hook; does nothing while an installation is active (even one Windows refused,
    // until stop()).
    virtual void start(Claims claims, OnButton on_button) = 0;
    // Removes the hook on its own thread (WM_QUIT, waiting at most 1 s); the events the hook
    // queued before this are still dispatched. It waits up to 1 s more for them; a callback still
    // running then finishes later, and destroying the source waits for it, since `on_button`
    // calls into the owner. So a callback may block on stop()'s caller only briefly, and never
    // on a thread that destroys the source.
    virtual void stop() = 0;
    // The hook thread of the current installation is alive.
    virtual bool running() const = 0;
    // The current installation's hook was installed within `timeout`.
    virtual bool
    wait_until_installed(std::chrono::milliseconds timeout = std::chrono::seconds(1)) = 0;
};

// The hook callback's decision (MouseButtonHook._make_callback), apart from Windows. It holds the
// set of buttons whose press was swallowed, which outlives a restart of the hook: a release whose
// press was swallowed before a rehook is swallowed too, or the app sees a lone XBUTTONUP.
// Affinity: decide() on one hook thread at a time; the rest thread-safe.
class MouseButtonFilter {
public:
    // GetAsyncKeyState for a virtual key.
    using KeyState = std::function<short(int vk)>;
    // Hands a swallowed event to the dispatch thread; it must not block.
    using Dispatch = std::function<void(const MouseEvent&)>;

    // `timer` (seconds, perf_counter) times each side-button callback.
    explicit MouseButtonFilter(MonotonicClock timer = steady_seconds());

    // One hook call: true when the hook swallows the message (returns 1), false when it calls
    // the next hook. Only X button messages are decided and timed; anything else passes untouched.
    // An exception from `claims` or `dispatch` lets the message through.
    bool decide(int code, unsigned message, std::uint32_t mouse_data, std::uint32_t flags,
                const KeyState& key_state, const MouseHookSource::Claims& claims,
                const MonotonicClock& clock, const Dispatch& dispatch);

    // The slowest side-button callback so far, in milliseconds.
    double worst_callback_ms() const { return worst_ms_.load(); }
    // _report_latency, on the dispatch thread after each event: records the metric
    // mouse_hook_callback_ms (rounded to 3 places) when the worst callback got slower.
    void report_latency();
    bool suppressed(MouseButton button) const;

    // A clock of seconds from std::chrono::steady_clock.
    static MonotonicClock steady_seconds();

private:
    // _real_side_button: nullopt for injected input and anything but XBUTTON1 or XBUTTON2.
    static std::optional<MouseButton> real_side_button(std::uint32_t mouse_data,
                                                       std::uint32_t flags);

    MonotonicClock timer_;
    std::array<std::atomic<bool>, 2> suppressed_{};
    std::atomic<double> worst_ms_{0.0};
    std::mutex report_mutex_;
    double reported_ms_ = 0.0;
};

// A real left or right click whose press began over another process's window: its release point
// (screen pixels) and the hook's monotonic time stamp (seconds). Called on the dispatch thread,
// never inside the hook.
using ClickObserver = std::function<void(int x, int y, double at)>;

// One observed click, as the hook queues it (_Click).
struct Click {
    int x = 0;
    int y = 0;
    double at = 0;

    friend bool operator==(const Click&, const Click&) = default;
};

// The click half of the hook callback (observe_click), apart from Windows. It remembers which of
// the left and right buttons went down over another process, until that button's release; a new
// installation starts with a new filter, as each Python callback has its own set. It never
// decides whether the hook swallows anything: every click reaches the app.
// Affinity: one hook thread.
class ClickFilter {
public:
    // Whether the window under (x, y) belongs to another process (WindowFromPoint and
    // GetWindowThreadProcessId): false when there is no window there or its owner is unknown.
    // Asked inside the hook for each real press; it must answer at once.
    using PressedOutside = std::function<bool(int x, int y)>;

    // The four button messages the filter reads.
    static bool reads(unsigned message);

    // One hook call: the click to queue when this is the real release of a press that began
    // outside, else nothing. Injected messages, negative codes and any other message change
    // nothing; a failing `outside` or `clock` gives nothing.
    std::optional<Click> observe(int code, unsigned message, int x, int y, std::uint32_t flags,
                                 const PressedOutside& outside, const MonotonicClock& clock);

    bool pressed_outside(unsigned down_message) const;

private:
    bool left_ = false;
    bool right_ = false;
};

// The platform's source. Windows: WH_MOUSE_LL on its own message-loop thread "fv-mouse-hook" and
// a dispatch thread "fv-mouse-dispatch", stamping events with `clock`. Elsewhere start() throws
// "Mouse button shortcuts are not supported on this platform."
std::unique_ptr<MouseHookSource> make_mouse_hook_source(MonotonicClock clock = monotonic_clock());

// The same source, also observing clicks (MouseButtonHook's on_click): `on_click` runs on the
// "fv-mouse-dispatch" thread for each click a ClickFilter keeps, and the hook still passes every
// click on. start() is as above; an empty Claims claims nothing and an empty OnButton is never
// called, so a walkthrough starts it with neither.
std::unique_ptr<MouseHookSource> make_mouse_hook_source(ClickObserver on_click,
                                                        MonotonicClock clock = monotonic_clock());

} // namespace fv::hotkeys
