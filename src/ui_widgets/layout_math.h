#pragma once

#include <span>
#include <vector>

#include "uimath/overlay_position.h"
#include "uimath/window_geometry.h"

// The layout decisions of the composite widgets, without Qt: ButtonRow's rows, SegmentedBar's
// columns and FlowLayout's lines. Sizes and positions are logical pixels.
namespace fv {

// ButtonRow._row_sizes: how many buttons go in each row, the fewest rows where every row fits
// room. Rows are balanced and the short ones come first (three buttons wrap 1 + 2); each row
// needs the sum of its buttons' widths plus spacing between them. When even one button per row
// does not fit, every button gets its own row. No buttons, no rows.
std::vector<int> button_row_sizes(std::span<const int> widths, int room, int spacing);

struct SegmentedShape {
    // Choices per row.
    int columns = 0;
    // Too narrow for one choice per row: one column, and the choices' text wraps.
    bool wrap = false;
    friend bool operator==(const SegmentedShape&, const SegmentedShape&) = default;
};

// SegmentedBar._reflow: n choices of width cell (the widest natural width) in a tray whose
// room is its inner width plus one gap. fits = max(1, min(n, room // (cell + gap))), and the
// rows are balanced, so four choices wrap 2 + 2, never 3 + 1: columns = ceil(n / ceil(n /
// fits)). No choices, no columns.
SegmentedShape segmented_columns(int n, int cell, int gap, int room);

// FlowLayout._arrange at the origin: items of the given sizes left to right, wrapping when the
// next would pass width, spacing between items and lines. Each item is centred on its line's
// height and never wider than width. The layout passes only its non-empty items.
std::vector<Rect> flow_arrange(std::span<const Size> sizes, int width, int spacing);

// The height flow_arrange's lines take: 0 without items.
int flow_height(std::span<const Size> sizes, int width, int spacing);

} // namespace fv
