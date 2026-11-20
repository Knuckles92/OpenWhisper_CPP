#pragma once

#include "uimath/overlay_position.h"
#include "uimath/window_geometry.h"

namespace fv {

// How far a dialog keeps from the screen's edges when it shrinks to fit: Wayland reports the
// whole output, bars included.
inline constexpr int kDialogScreenMargin = 32;

// The smallest a Settings window may be when the desktop session is an Omarchy one.
inline constexpr Size kOmarchyDialogMinimum{360, 240};

struct FitResult {
    // The minimum size to set: the given minimum, lowered to what the screen has room for.
    Size minimum;
    // The size the window ends up: the given size, shrunk to the room.
    Size size;
    // Whether size differs from the one passed in, so the window has to be resized.
    bool resized = false;
};

// SettingsDialog._fit_to_screen: shrink a window to the screen it opens on when it does not fit.
// The room is the available geometry less kDialogScreenMargin on each side (at least 1x1); the
// minimum is lowered to it, so a small screen can still hold the window, and the size is cut to
// it. window and minimum are the dialog's size now and its normal minimum (kOmarchyDialogMinimum
// in an Omarchy session, else the dialog's own).
FitResult fit_to_screen(Size window, Size minimum, const Rect& available);

} // namespace fv
