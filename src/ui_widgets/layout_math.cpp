#include "ui_widgets/layout_math.h"

#include <algorithm>
#include <cstddef>
#include <numeric>

namespace fv {
namespace {

// Python's // on ints: rounds toward negative infinity.
int floor_div(int a, int b) {
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int ceil_div(int a, int b) {
    return -floor_div(-a, b);
}

struct Line {
    std::size_t first = 0;
    std::size_t count = 0;
    int height = 0;
};

std::vector<Line> flow_lines(std::span<const Size> sizes, int width, int spacing) {
    std::vector<Line> lines;
    Line line;
    int used = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const Size& hint = sizes[i];
        if (line.count > 0 && used + spacing + hint.width > width) {
            lines.push_back(line);
            line = Line{i, 0, 0};
            used = 0;
        }
        if (line.count == 0) {
            line.first = i;
        }
        used += (line.count > 0 ? spacing : 0) + hint.width;
        line.height = line.count > 0 ? std::max(line.height, hint.height) : hint.height;
        ++line.count;
    }
    if (line.count > 0) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace

std::vector<int> button_row_sizes(std::span<const int> widths, int room, int spacing) {
    const int count = static_cast<int>(widths.size());
    for (int rows = 1; rows <= count; ++rows) {
        const int base = count / rows;
        const int extra = count % rows;
        std::vector<int> sizes(static_cast<std::size_t>(rows - extra), base);
        sizes.insert(sizes.end(), static_cast<std::size_t>(extra), base + 1);
        std::size_t start = 0;
        bool fits = true;
        for (const int size : sizes) {
            const auto taken = widths.subspan(start, static_cast<std::size_t>(size));
            if (std::accumulate(taken.begin(), taken.end(), 0) + spacing * (size - 1) > room) {
                fits = false;
                break;
            }
            start += static_cast<std::size_t>(size);
        }
        if (fits) {
            return sizes;
        }
    }
    return std::vector<int>(static_cast<std::size_t>(count), 1);
}

SegmentedShape segmented_columns(int n, int cell, int gap, int room) {
    if (n <= 0 || cell + gap <= 0) {
        return {};
    }
    const int fits = std::max(1, std::min(n, floor_div(room, cell + gap)));
    const int rows = ceil_div(n, fits);
    return {ceil_div(n, rows), cell + gap > room};
}

std::vector<Rect> flow_arrange(std::span<const Size> sizes, int width, int spacing) {
    std::vector<Rect> rects;
    rects.reserve(sizes.size());
    int y = 0;
    for (const Line& line : flow_lines(sizes, width, spacing)) {
        int x = 0;
        for (std::size_t i = line.first; i < line.first + line.count; ++i) {
            const Size& hint = sizes[i];
            rects.push_back({x, y + floor_div(line.height - hint.height, 2),
                             std::min(hint.width, width), hint.height});
            x += hint.width + spacing;
        }
        y += line.height + spacing;
    }
    return rects;
}

int flow_height(std::span<const Size> sizes, int width, int spacing) {
    const std::vector<Line> lines = flow_lines(sizes, width, spacing);
    if (lines.empty()) {
        return 0;
    }
    int y = 0;
    for (const Line& line : lines) {
        y += line.height + spacing;
    }
    return y - spacing;
}

} // namespace fv
