#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "focus/snapshot.h"

// Synthetic keystrokes: the paste after a dictation, and the copy that reads the selection. A
// copy is only ever sent after the hotkey's own modifiers are up, and never to a terminal, where
// Ctrl+C interrupts the running program. Python's services/synthetic_keys.py and
// _hotkey_keyboard.send_paste.
namespace fv::focus {

// How long a copy waits by default for the shortcut's modifiers to come up.
inline constexpr std::chrono::milliseconds MODIFIER_WAIT{800};
inline constexpr std::chrono::milliseconds MODIFIER_POLL{10};
// Where the held keys can't be read, how long a copy waits for the hand to come off the
// shortcut instead.
inline constexpr std::chrono::milliseconds BLIND_WAIT{150};

// This desktop can't send keystrokes to other apps.
class SyntheticKeysError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Affinity: thread-safe. wait_for_modifiers_released blocks: never call it on the GUI thread.
class SyntheticKeys {
public:
    virtual ~SyntheticKeys() = default;
    // Ctrl+V to the focused app. Throws SyntheticKeysError where keystrokes can't be sent.
    virtual void send_paste() = 0;
    // Ctrl+C to the focused app. Throws SyntheticKeysError where keystrokes can't be sent.
    virtual void send_copy() = 0;
    // Checks once without waiting; false where key state can't be read.
    virtual bool modifiers_held() = 0;
    // Waits until no modifier key is held; false when one still is at the timeout or the wait
    // was cancelled. A key state that can't be read counts as released (after BLIND_WAIT).
    virtual bool wait_for_modifiers_released(std::chrono::milliseconds timeout = MODIFIER_WAIT,
                                             const CancelToken& cancel = {}) = 0;
};

// One keyboard event as SendInput takes it: a virtual key, its scan code, down or up.
struct KeyStroke {
    std::uint16_t vk = 0;
    std::uint16_t scan = 0;
    bool up = false;

    bool operator==(const KeyStroke&) const = default;
};

inline constexpr std::uint16_t VK_CONTROL_KEY = 0x11;
inline constexpr std::uint16_t VK_MENU_KEY = 0x12;
inline constexpr std::uint16_t VK_SHIFT_KEY = 0x10;
inline constexpr std::uint16_t VK_LWIN_KEY = 0x5B;
inline constexpr std::uint16_t VK_RWIN_KEY = 0x5C;

// What NativeSyntheticKeys needs from the OS keyboard; a fake in tests.
// Affinity: thread-safe.
class KeyboardPort {
public:
    virtual ~KeyboardPort() = default;
    // Injects the strokes as one batch; returns how many the OS took.
    virtual std::size_t send(std::span<const KeyStroke> strokes) = 0;
    virtual std::uint16_t scan_code(std::uint16_t vk) = 0;
    // Whether key_down reads the OS's own key state (GetAsyncKeyState), which also sees
    // virtual-key input without a hardware scan code.
    virtual bool reads_key_state() const = 0;
    virtual bool key_down(std::uint16_t vk) = 0;
};

// The keyboard hook's view of a modifier ("ctrl", "alt", "shift", "windows"), held or not.
using HookKeyState = std::function<bool(std::string_view name)>;
// Sleeps for the time; true when the wait was cancelled instead.
using CancellableSleep = std::function<bool(std::chrono::milliseconds)>;

// The press-and-release of modifier+key in the order the keyboard library sends a chord: the
// modifier down, the key down, the key up, the modifier up.
std::vector<KeyStroke> chord_strokes(KeyboardPort& port, std::uint16_t modifier_vk,
                                     std::uint16_t key_vk);
// The modifiers the OS holds ("ctrl", "alt", "shift", "windows"), including virtual-key input
// with no hardware scan code; nullopt where the OS key state can't be read.
std::optional<std::set<std::string>>
native_windows_modifiers(const std::function<bool(std::uint16_t vk)>& physical);
// The hook keeps a modifier down when its key-up went to an elevated window, and may miss
// virtual-key modifiers from accessibility tools: the OS state has the last word in both
// directions; without it the hook decides.
bool windows_modifiers_held(const std::function<bool(std::uint16_t vk)>& physical,
                            const HookKeyState& hook);
// The polling of wait_for_modifiers_released over a probe (empty: key state unreadable; a
// probe that throws counts as released), a clock and a sleep.
bool wait_until_released(const std::function<bool()>& probe, std::chrono::milliseconds timeout,
                         const Clock& clock, const CancellableSleep& sleep);

// Synthetic keys over a KeyboardPort: the Windows implementation, with the real port from
// make_keyboard_port(). Affinity: thread-safe.
class NativeSyntheticKeys final : public SyntheticKeys {
public:
    // hook: the keyboard hook's modifier state, consulted only where the OS's can't be read.
    // clock and sleep default to the system clock and the cancel token's wait.
    explicit NativeSyntheticKeys(std::shared_ptr<KeyboardPort> port, HookKeyState hook = {},
                                 const Clock* clock = nullptr, CancellableSleep sleep = {});

    void send_paste() override;
    void send_copy() override;
    bool modifiers_held() override;
    bool wait_for_modifiers_released(std::chrono::milliseconds timeout = MODIFIER_WAIT,
                                     const CancelToken& cancel = {}) override;

private:
    std::function<bool()> probe() const;
    void send_chord(std::uint16_t key_vk);

    std::shared_ptr<KeyboardPort> port_;
    HookKeyState hook_;
    const Clock* clock_;
    CancellableSleep sleep_;
};

// The OS keyboard (SendInput, MapVirtualKeyW, GetAsyncKeyState); throws SyntheticKeysError
// where there is none. Every INPUT it sends carries kSyntheticInputTag (base/input_tag.h).
std::shared_ptr<KeyboardPort> make_keyboard_port();
// This computer's synthetic keys.
std::unique_ptr<SyntheticKeys> make_synthetic_keys();

// The app catalogue's surface for identity ("text", "code", "terminal"), as the app styles see
// it: "" for our own windows and apps without an id.
std::string catalog_surface(const AppIdentity& identity);
// A replacement for catalog_surface (tests); one that throws counts as "".
using SurfaceLookup = std::function<std::string(const AppIdentity&)>;

// Whether identity is a terminal. The app catalogue is the one list of terminals.
bool is_terminal(const std::optional<AppIdentity>& identity, const SurfaceLookup& surface = {});
// Whether the app copies the whole line when nothing is selected (VS Code family, Sublime, Zed,
// Visual Studio, JetBrains IDEs, or a code surface), so a copied line is not a selection.
bool copies_line_without_selection(const std::optional<AppIdentity>& identity,
                                   const SurfaceLookup& surface = {});

} // namespace fv::focus
