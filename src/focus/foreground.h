#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "focus/snapshot.h"

// The foreground app on Windows, from plain Win32 calls (no UI Automation). Takes well under a
// millisecond, so it runs on whichever thread asks. The window title is read into memory only to
// name a browser's site. Python's services/focus_context/_win.py.
namespace fv::focus {

// The Win32 calls foreground_identity makes; a fake in tests. Handles are HWND values.
// Affinity: thread-safe.
class Win32Desktop {
public:
    virtual ~Win32Desktop() = default;
    // GetForegroundWindow; 0 when none.
    virtual std::uintptr_t foreground_window() = 0;
    // GetWindowThreadProcessId; 0 when unknown.
    virtual std::uint32_t window_pid(std::uintptr_t window) = 0;
    // GetClassNameW (256), or "".
    virtual std::string class_name(std::uintptr_t window) = 0;
    // GetWindowTextW, at most WINDOW_TITLE_CHARS UTF-16 units. Another process's caption comes
    // from the window structure without a message to that process, so a hung app cannot block.
    virtual std::string title(std::uintptr_t window) = 0;
    // The lowercase executable name of pid, or "" when access is denied.
    virtual std::string image_name(std::uint32_t pid) = 0;
    // The packaged app's CoreWindow child under an ApplicationFrameHost frame, when it belongs to
    // another process than the frame: (window, pid), or nullopt.
    virtual std::optional<std::pair<std::uintptr_t, std::uint32_t>>
    uwp_app_window(std::uintptr_t frame, std::uint32_t frame_pid) = 0;
};

inline constexpr int WINDOW_TITLE_CHARS = 512;

// The app owning the foreground window, or nullopt during focus changes. own_pid is this
// process: its windows are named as the product's own (is_self).
std::optional<AppIdentity> foreground_identity(Win32Desktop& desktop, std::uint32_t own_pid);
// The same through the real Win32 API; nullopt off Windows.
std::optional<AppIdentity> foreground_identity();
// The real Win32 API; throws std::runtime_error off Windows.
std::unique_ptr<Win32Desktop> make_win32_desktop();

// Our own identity's app id and name: the product name, lowercased for the id.
std::string self_app_id();
std::string self_app_name();

} // namespace fv::focus
