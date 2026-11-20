#pragma once

#include <string>
#include <utility>

#include <QString>

// Where Snap can capture the screen, and through what (ui_qt/snap/support.py). Windows and X11
// hand the screen's pixels to Qt, and so does macOS once Screen Recording is granted; Hyprland
// needs grim; other Wayland desktops are not supported.
namespace fv::ui_snap {

// What snap_support decides by. currentPlatform() reads this desktop.
struct Platform {
    std::string system;           // sys.platform: "win32", "darwin", "linux", "freebsd14"
    bool hyprland = false;        // services/hyprland.py available(): its socket and hyprctl
    bool grim = false;            // grim is on PATH
    bool wayland = false;         // a Wayland Qt platform or session
    bool screenRecording = false; // macOS granted Screen Recording
};

// The reasons, verbatim; the macOS one names the product.
QString macReason();
inline constexpr char16_t kWaylandReason[] =
    u"Snap doesn't work on this Wayland desktop yet. It works on Windows, X11 and Hyprland.";
inline constexpr char16_t kGrimReason[] =
    u"Snap on Hyprland needs grim. Install the grim package, then try again.";
inline constexpr char16_t kOtherReason[] = u"Snap isn't available on this system yet.";

// This desktop: systemPlatform() and, off Windows, the Hyprland, grim and Wayland probes. The
// macOS Screen Recording grant is not read (macOS is not ported), so it reads as not granted.
Platform currentPlatform();

// Whether captures go through grim: a Linux Hyprland session.
bool usesGrim(const Platform& platform);
bool usesGrim();

// (supported, reason); reason is a short user-facing sentence, or "". Never prompts.
std::pair<bool, QString> snapSupport(const Platform& platform);
std::pair<bool, QString> snapSupport();

// Asks for what snapSupport found missing where the system can be asked: only macOS, once per
// run, which is not ported, so this does nothing.
void requestAccess();

} // namespace fv::ui_snap
