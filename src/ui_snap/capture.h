#pragma once

#include <optional>

#include <QImage>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QWidget>

class QScreen;

// Grabbing the screen under the pointer without our own windows in the shot
// (ui_qt/snap/capture.py). Pixels come through the ScreenGrabber seam (ui_snap/desktop.h).
// Affinity: GUI thread.
namespace fv::ui_snap {

// At least two frames at 60 Hz, with additional compositor settling below.
inline constexpr int kMinSettleMs = 34;
// Hyprland plays a close animation from a snapshot of the window (about 150 ms) and grim
// would capture it mid-fade, so a hidden window gets this long to be gone.
inline constexpr int kHyprlandSettleMs = 200;
// Cocoa's translucent Tool windows can remain in the composed image after orderOut reports them
// hidden.
inline constexpr int kMacosSettleMs = 300;

// The screen the user is pointing at: the one Snap captures and covers; the primary screen
// when the pointer is on none. (On Hyprland OpenWhisper asks grim for the focused output, which
// is not ported.)
QScreen* screenUnderCursor();

// The screen of a mouse-hook event, in Windows' native coordinates: the monitor there, matched
// by its desktop origin (Qt scales screen sizes on Windows but leaves their origins intact).
// nullptr when there is none or it cannot be read.
QScreen* screenAtWindowsPoint(QPoint point);

// Resolves a native click without following the pointer after its release.
QScreen* screenAtClick(QPoint point);

// Every screen a snap freezes, the pointer's first: the user may select on any. Hyprland
// fullscreens each window on the focused monitor whatever screen it asks for, so there a snap
// covers that one alone.
QList<QScreen*> screensToCover();

// The frozen screen for a snap, tagged with its pixel ratio; nullopt on failure. Formats other
// than RGB32, ARGB32 and ARGB32_Premultiplied become ARGB32_Premultiplied, so it can be painted
// on. The ratio comes from the captured width against the screen's logical width rather than
// from the screen alone, so a physical-pixel capture still maps one image pixel to one device
// pixel.
std::optional<QImage> grab(QScreen* screen);

// How long hidden windows take to leave the desktop's composed image, in ms: two frames at
// the screen's refresh rate (34 ms below 24 Hz), at least 34 ms, 200 ms through grim and
// 300 ms on cocoa.
int settleMs(QScreen* screen);
// The same decided from a refresh rate in Hz (0 for no screen), grim and the cocoa platform.
int settleMs(double refreshRate, bool grim, bool cocoa);

// Hides the top-level windows of windows that would show up in a capture and returns the ones
// hidden: not those kept out by affinity (no flicker), hidden or minimised. Qt's own hide, not a
// subclass's: the recording overlay's hide ends the session it shows.
QList<QPointer<QWidget>> hideWindows(const QList<QPointer<QWidget>>& windows);

// Shows windows hidden for a capture again, without taking the focus back.
void restoreWindows(const QList<QPointer<QWidget>>& windows);

} // namespace fv::ui_snap
