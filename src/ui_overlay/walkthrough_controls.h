#pragma once

#include <vector>

#include <QFont>
#include <QRectF>
#include <QString>

class QPainter;

namespace fv {

// A walkthrough's Snap and Done on the recording overlay (waveform_overlay.py:1042-1185): where
// they go along the badge row, and how each is painted. The overlay keeps the state, the clicks
// and the signals; this only lays out and draws.

enum class WalkthroughControl { Snap, Finish };

// The gap between neighbours along the badge row, and the icon's size (px).
inline constexpr int kControlGap = 5;
inline constexpr int kControlIcon = 12;

// "snap" or "finish": what the overlay's hover and hit report.
QString walkthroughControlKey(WalkthroughControl control);
// "Snap" or "Done".
QString walkthroughControlLabel(WalkthroughControl control);

// The controls' words: Segoe UI 8 pt DemiBold.
QFont walkthroughControlFont();

// A control's width: 26 px for the icon alone, else the icon, its padding and the word measured
// in walkthroughControlFont() (labelWidth, horizontalAdvance).
double walkthroughControlWidth(bool labelled, int labelWidth);

// What the row holds besides the controls.
struct WalkthroughControlsInput {
    int width = 0;
    bool chipVisible = false;
    double chipWidth = 0.0;
    bool badgeShown = false;
    double badgeWidth = 0.0;
    // horizontalAdvance of "Snap" and "Done" in walkthroughControlFont().
    int snapLabelWidth = 0;
    int doneLabelWidth = 0;
};

struct PlacedControl {
    WalkthroughControl control = WalkthroughControl::Snap;
    QRectF rect;
    // The word drawn beside the icon, or empty when the controls dropped their words.
    QString label;
};

// _layout_controls: right to left before the language chip, Done then Snap (Python's order),
// each 18 px tall on the badge row; both drop their words for icons when the two labelled ones
// do not fit between the badge and the chip.
std::vector<PlacedControl> layoutWalkthroughControls(const WalkthroughControlsInput& in);

// The same, measuring the words.
std::vector<PlacedControl> layoutWalkthroughControls(int width, bool chipVisible, double chipWidth,
                                                     bool badgeShown, double badgeWidth);

// _draw_control: Snap on an accent fill with a camera, Done a ghost chip with a check; hover
// brightens either. radius is the badge's corner radius.
void drawWalkthroughControl(QPainter& painter, const PlacedControl& placed, bool hover,
                            double radius);

} // namespace fv
