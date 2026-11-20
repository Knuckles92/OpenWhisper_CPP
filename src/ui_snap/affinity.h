#pragma once

#include <cstdint>

#include <QList>
#include <QPointer>
#include <QWidget>

// Window plumbing for Snap: capture exclusion and foreground focus (ui_qt/snap/affinity.py).
// Windows 10 2004 and later can leave a window out of every screen capture
// (SetWindowDisplayAffinity with WDA_EXCLUDEFROMCAPTURE): the user still sees it, but captured
// pixels show what is behind it. Elsewhere Snap hides our windows for the moment of capture.
// Every call goes through the Win32Desktop seam (ui_snap/desktop.h). Affinity: GUI thread.
namespace fv::ui_snap {

inline constexpr std::uint32_t kWdaNone = 0x0;
inline constexpr std::uint32_t kWdaExcludeFromCapture = 0x11;

// Leaves window's top-level window out of screen captures on Windows, or puts it back. Returns
// true when the system applied the change; elsewhere does nothing and returns false. The
// native handle the exclusion was applied to is remembered: Qt can recreate a window's native
// handle (new flags, translucency), which drops the exclusion, so a stale handle reads as not
// excluded. A refusal is not remembered.
bool keepOutOfScreenshots(QWidget* window, bool enabled);

// Whether keepOutOfScreenshots currently excludes window's top-level window from captures.
bool isKeptOut(QWidget* window);

// Makes window the foreground window though another application has the focus: Snap opens
// from a global hotkey while the user works elsewhere, and Windows lets only the foreground
// process hand out focus. Attaches to the foreground thread's input for the call, as Qt does
// under its AlwaysActivateWindow option. Only on the "windows" Qt platform.
void bringToFront(QWidget* window);

// The window to give the focus back to after live drawing: GetForegroundWindow on the
// "windows" Qt platform, else 0 (macOS is not ported).
WId foregroundWindow();

// Returns the focus to previous when one of windows (our temporary drawing surfaces) still
// has it.
void returnForeground(WId previous, const QList<QPointer<QWidget>>& windows);

} // namespace fv::ui_snap
