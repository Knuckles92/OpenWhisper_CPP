#pragma once

namespace fv {

// Screen geometry in logical pixels, with QRect's meaning: x and y are the top-left corner,
// and a rectangle is null when width and height are both 0.
struct Point {
    int x = 0;
    int y = 0;
    friend bool operator==(const Point&, const Point&) = default;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool is_null() const { return width == 0 && height == 0; }
    // QRect::contains(QRect): other lies inside, edges included; false when either is empty.
    bool contains(const Rect& other) const;
    friend bool operator==(const Rect&, const Rect&) = default;
};

// The top-left (x, y) clamped so a width x height rectangle stays margin pixels inside
// available (the screen's available geometry, without taskbars). A rectangle larger than the
// usable area is pinned to the top-left inset. A non-positive size or an empty screen leaves
// the position as given.
Point clamp_rect_to_available(int x, int y, int width, int height, const Rect& available,
                              int margin = 8);

// Where an overlay goes near anchor (the cursor, global coordinates): below-right by offset,
// flipped left and/or above when it would pass the right or bottom edge less margin, then
// clamped into available.
Point preferred_overlay_position(Point anchor, int width, int height, const Rect& available,
                                 int offset = 10, int margin = 8);

// The tallest overlay that fits beside anchor, above or below whichever has more room, never
// more than configured_max and never below 1.
int max_height_for_anchor(Point anchor, const Rect& available, int configured_max, int offset = 10,
                          int margin = 8);

// Where a window's frame goes so that all of it is on available: left where it is when it
// already fits, otherwise centred, never with its top-left corner (the title bar's) off it.
Point on_screen_position(const Rect& frame, const Rect& available);

} // namespace fv
