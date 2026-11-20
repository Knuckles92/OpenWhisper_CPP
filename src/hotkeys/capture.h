#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/signal.h"
#include "hotkeys/keyboard_hook.h"

// Capturing one shortcut from the OS hook for a settings field (ui_qt/widgets/hotkey_capture.py
// HotkeyCaptureThread, the Windows variant), so keys the app's own hook would swallow are seen.
namespace fv::hotkeys {

// HOTKEY_CAPTURE_FAILURE_MESSAGE on Windows.
inline constexpr std::string_view kHotkeyCaptureFailureMessage =
    "Could not capture hotkey. Please try again.";

// One captured key as the capture keeps it.
struct CapturedKey {
    std::optional<std::string> name; // `keyboard`'s event name; nullopt when it has none
    bool keypad = false;
    bool down = true;
};

// keyboard.get_hotkey_name(names): each name normalized, "left " and "right " dropped and "+"
// spelled "plus", duplicates removed, then ctrl, alt, shift and windows first in that order and
// the rest sorted, joined by "+". Throws std::invalid_argument for an empty name, as
// keyboard.normalize_name raises.
std::string get_hotkey_name(const std::vector<std::string>& names);

struct CaptureOutcome {
    enum class Kind { Pending, Captured, Cancelled };
    Kind kind = Kind::Pending;
    std::string hotkey; // Captured: the shortcut

    friend bool operator==(const CaptureOutcome&, const CaptureOutcome&) = default;
};

// A Windows capture's verdict on the keys seen so far: pending until the first key-up; then
// cancelled when every key was Escape, else captured: the keys named as `keyboard` names them
// (a keypad key as "kp_<name>") through get_hotkey_name, then parse_hotkey and format_hotkey.
// Throws std::invalid_argument for a key with no name, which fails the capture.
CaptureOutcome capture_outcome(const std::vector<CapturedKey>& keys);

// One capture, run on a thread of its own. Each start() captures one shortcut and ends with
// exactly one of captured, cancelled or failed, then finished; after stop() it ends silently
// (finished only). Signals are emitted on the capture thread and reach each subscriber on its
// executor.
// Affinity: start, stop, running and wait from one thread (the settings page's).
class HotkeyCapture {
public:
    virtual ~HotkeyCapture() = default;

    virtual void start() = 0;
    // Ends the capture without a result.
    virtual void stop() = 0;
    // QThread.isRunning: a started capture has not ended yet.
    virtual bool running() const = 0;
    // QThread.wait(timeout): true once the capture has ended (or never started).
    virtual bool wait(std::chrono::milliseconds timeout) = 0;

    Signal<std::string> captured;
    Signal<> cancelled;
    Signal<std::string> failed;
    Signal<> finished;
};

// Builds the capture a field starts; widget tests pass one that makes FakeCaptures.
using CaptureFactory = std::function<std::unique_ptr<HotkeyCapture>()>;

// A capture over its own keyboard hook, which it installs for each capture and removes after,
// never touching any other hook. The hook's callback always lets keys through, so the app's own
// suppressing hook keeps working and a canceled capture leaves nothing behind. The thread is
// "fv-hotkey-capture".
std::unique_ptr<HotkeyCapture> make_hotkey_capture(std::unique_ptr<KeyboardHook> hook);

// The platform's capture hook: on Windows a WH_KEYBOARD_LL hook of its own, beside the app's.
// Elsewhere installing it throws, so a capture fails.
std::unique_ptr<KeyboardHook> make_capture_hook();

// make_hotkey_capture(make_capture_hook()).
std::unique_ptr<HotkeyCapture> make_hotkey_capture();

} // namespace fv::hotkeys
