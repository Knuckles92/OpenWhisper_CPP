#pragma once

class QWidget;

// The walkthrough windows that must never show up in a screenshot (the Snap menu, the flying
// screenshot) leave themselves out through Snap's capture exclusion. Affinity: GUI thread.
namespace fv {

// ui_snap::keepOutOfScreenshots: Windows leaves window's top-level window out of every capture;
// elsewhere Snap hides our windows for the moment of capture instead, and this does nothing.
// Returns whether the system applied the change.
bool setKeepOutOfScreenshots(QWidget* window, bool enabled);

} // namespace fv
