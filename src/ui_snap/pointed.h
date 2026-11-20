#pragma once

#include <optional>

#include <QList>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QWidget>

#include "ui_snap/desktop.h"

class QScreen;

// The window a snap of "the window under the pointer" keeps, and the window edges selections
// snap to (ui_qt/snap/pointed.py). Windows asks the window manager through the WindowLister
// seam; macOS (Quartz) is not ported, and other desktops have no answer. Affinity: GUI thread.
namespace fv::ui_snap {

// Whether this desktop can say which window is under the pointer: the "windows" and "cocoa" Qt
// platforms. On cocoa the answers below are still empty, because Quartz is not ported.
bool pointedSupported();

// The window to keep, in screen's logical pixels from its top left and clipped to it, or
// nullopt. That is the window under the pointer, unless front asks for the window in front, as
// does the pointer on one of ours (our own windows, which never take the focus).
std::optional<QRect> windowUnderPointer(QScreen* screen, const QList<QPointer<QWidget>>& ours = {},
                                        bool front = false);

// Visible window frames before a drawing surface covers the desktop, front to back, in
// screen's logical pixels: visible, not minimised and not cloaked windows other than ours,
// frames under 24 px skipped, and only those with a part on screen not covered by a window in
// front of them.
QList<QRect> windowTargets(QScreen* screen, const QList<QPointer<QWidget>>& ours = {});

// A native frame in a screen's logical pixels: physical pixels from origin, divided by scale and
// rounded half to even.
QRect logicalFrame(const NativeRect& frame, QPoint origin, double scale);

} // namespace fv::ui_snap
