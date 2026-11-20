#include "uimath/overlay_position.h"

#include <algorithm>

namespace fv {
namespace {

// Python's // : rounds toward negative infinity.
int floor_div(int a, int b) {
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// QRect keeps x1..x2 inclusive (x2 = x + width - 1) and normalises a negative extent when it
// compares; these are its left and right (or top and bottom) as contains() reads them.
void span(int start, int extent, int& low, int& high) {
    const int end = start + extent - 1;
    if (end - start + 1 < 0) {
        low = end;
        high = start;
    } else {
        low = start;
        high = end;
    }
}

} // namespace

bool Rect::contains(const Rect& other) const {
    if (is_null() || other.is_null()) {
        return false;
    }
    int l1 = 0;
    int r1 = 0;
    int l2 = 0;
    int r2 = 0;
    span(x, width, l1, r1);
    span(other.x, other.width, l2, r2);
    if (l2 < l1 || r2 > r1) {
        return false;
    }
    int t1 = 0;
    int b1 = 0;
    int t2 = 0;
    int b2 = 0;
    span(y, height, t1, b1);
    span(other.y, other.height, t2, b2);
    return !(t2 < t1 || b2 > b1);
}

Point clamp_rect_to_available(int x, int y, int width, int height, const Rect& available,
                              int margin) {
    if (width <= 0 || height <= 0 || available.is_null() || available.width <= 0) {
        return {x, y};
    }
    const int min_x = available.x + margin;
    const int min_y = available.y + margin;
    // A rectangle larger than the usable area is pinned to the top-left inset.
    const int max_x = std::max(min_x, available.x + available.width - width - margin);
    const int max_y = std::max(min_y, available.y + available.height - height - margin);
    return {std::max(min_x, std::min(x, max_x)), std::max(min_y, std::min(y, max_y))};
}

Point preferred_overlay_position(Point anchor, int width, int height, const Rect& available,
                                 int offset, int margin) {
    int x = anchor.x + offset;
    int y = anchor.y + offset;
    const int right_limit = available.x + available.width - margin;
    const int bottom_limit = available.y + available.height - margin;
    if (x + width > right_limit) {
        x = anchor.x - width - offset;
    }
    if (y + height > bottom_limit) {
        y = anchor.y - height - offset;
    }
    return clamp_rect_to_available(x, y, width, height, available, margin);
}

int max_height_for_anchor(Point anchor, const Rect& available, int configured_max, int offset,
                          int margin) {
    if (available.is_null() || available.height <= 0) {
        return std::max(1, configured_max);
    }
    const int below = available.y + available.height - margin - (anchor.y + offset);
    const int above = anchor.y - offset - (available.y + margin);
    const int room = std::max(below, above);
    return std::max(1, std::min(configured_max, room));
}

Point on_screen_position(const Rect& frame, const Rect& available) {
    if (available.contains(frame)) {
        return {frame.x, frame.y};
    }
    return {std::max(available.x, available.x + floor_div(available.width - frame.width, 2)),
            std::max(available.y, available.y + floor_div(available.height - frame.height, 2))};
}

} // namespace fv
