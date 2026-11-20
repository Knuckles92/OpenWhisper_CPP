#include "uimath/overlay_layout.h"

#include <algorithm>
#include <cmath>

#include "uimath/color.h"
#include "uimath/overlay_look.h"

namespace fv {
namespace {

// qRound(double).
int round_half_away(double value) {
    return value >= 0.0 ? static_cast<int>(value + 0.5) : static_cast<int>(value - 0.5);
}

} // namespace

Rect to_rect(const RectD& rect) {
    const int x = round_half_away(rect.x);
    const int y = round_half_away(rect.y);
    return Rect{x, y, round_half_away(rect.width + (rect.x - x) / 2),
                round_half_away(rect.height + (rect.y - y) / 2)};
}

bool top_row_in_use(const TopRowInput& in) {
    return in.badge_shown || (in.listening && in.chip_visible) || in.controls_clickable;
}

int band_top(const TopRowInput& in) {
    return top_row_in_use(in) ? kTopRow : 0;
}

Span top_row_span(const TopRowInput& in) {
    const double left = in.badge_shown ? kBadgeMargin + in.badge_width + 8 : kTextMargin;
    double right = in.width - 4;
    if (in.listening && in.chip_visible) {
        right = in.width - kBadgeMargin - in.chip_width;
    }
    if (in.controls_clickable) {
        right = in.controls_left;
    }
    return Span{left, right - 8};
}

bool live_shares_top_row(const TopRowInput& in, std::optional<double> live_label_width) {
    const Span span = top_row_span(in);
    const double label = live_label_width ? *live_label_width + 10 : 0.0;
    return span.right - span.left >= label + kHeaderLookMinWidth;
}

int header_height(const TopRowInput& in, std::optional<double> live_label_width) {
    return live_shares_top_row(in, live_label_width) ? kHeaderHeight : kHeaderHeight + kLiveRow;
}

Margins preview_text_margins(PreviewLayout layout, int base_height, bool top_row_in_use,
                             int header_height) {
    if (layout == PreviewLayout::Footer) {
        const int above = top_row_in_use ? kBadgeMargin + kBadgeHeight + 8 : 10;
        return Margins{above, base_height + 4};
    }
    return Margins{header_height + 6, 12};
}

Rect preview_text_rect(int width, int height, Margins margins) {
    return Rect{kTextMargin, margins.above, width - 2 * kTextMargin,
                std::max(20, height - margins.above - margins.below)};
}

int divider_y(PreviewLayout layout, int height, int base_height, int header_height) {
    return layout == PreviewLayout::Footer ? footer_band_top(height, base_height) : header_height;
}

Rect status_rect(bool has_words, PreviewLayout layout, int width, int height, int band_top,
                 int base_height, const RectD& header_room) {
    if (has_words) {
        if (layout == PreviewLayout::Footer) {
            return Rect{0, height - kStatusFromBottom, width, kStatusHeight};
        }
        return to_rect(RectD{header_room.x - kStatusInset, header_room.y,
                             header_room.width + 2 * kStatusInset, header_room.height});
    }
    return Rect{0, band_top + base_height - kStatusFromBottom, width, kStatusHeight};
}

HeaderRow header_row(const HeaderRowInput& in) {
    double center_y = kBadgeMargin + kBadgeHeight / 2.0;
    double left;
    double right;
    if (in.shares_top_row) {
        left = in.top_row_span.left;
        right = in.top_row_span.right;
    } else {
        center_y += kLiveRow;
        left = kTextMargin;
        right = in.width - kTextMargin;
    }
    bool clock = in.clock;
    double width = in.label_width;
    if (clock && width > right - left) {
        // Snap and Done crowd the row: the dot alone still says it is live.
        clock = false;
        width = in.label_width_without_clock;
    }
    HeaderRow row;
    if (in.live && (in.dot || clock) && width <= right - left) {
        row.draw_label = true;
        row.label_clock = clock;
        row.label_rect = RectD{left, center_y - 10, width, 20};
        left += width + 10;
    }
    row.room = RectD{left, center_y - kHeaderLookHeight / 2, detail::py_max(0.0, right - left),
                     kHeaderLookHeight};
    row.draw_look = !(in.caption || row.room.width < kHeaderLookMinWidth);
    return row;
}

SmallLook small_look(std::string_view small, const RectD& area, double overlay_width) {
    double scale = area.height / kLookHeight;
    SmallLook look;
    look.clip = area;
    look.origin_x = area.x;
    look.origin_y = area.y;
    if (small == kSmallFit) {
        scale = detail::py_min(scale, area.width / overlay_width);
        look.origin_x += (area.width - overlay_width * scale) / 2;
        look.origin_y += (area.height - kLookHeight * scale) / 2;
        look.scale_x = look.scale_y = scale;
        return look;
    }
    if (small == kSmallSquash) {
        look.scale_x = 1.0;
        look.scale_y = scale;
        look.band_width = area.width / 1.0;
        return look;
    }
    look.scale_x = look.scale_y = scale;
    look.band_width = area.width / scale;
    return look;
}

VisibleLines visible_preview_lines(const std::vector<double>& line_heights, const Rect& text_rect) {
    double height = 0.0;
    std::size_t first = line_heights.size();
    while (first > 0) {
        const double line = line_heights[first - 1];
        if (height + line > text_rect.height) {
            break;
        }
        height += line;
        --first;
    }
    // QRect::bottom() is the last row, y + height - 1.
    const double bottom = text_rect.y + text_rect.height - 1;
    return VisibleLines{first, bottom - height + 1, height};
}

int target_height(const HeightInput& in) {
    if (!in.has_words) {
        return in.base_height + in.band_top;
    }
    return std::min(in.max_height,
                    std::max(in.base_height, in.margins.above + in.text_height + in.margins.below));
}

int effective_max_height(const MaxHeightInput& in) {
    if (in.embedded) {
        return std::min(in.configured_max, std::max(in.base_height, in.parent_height / 3));
    }
    if (in.placed && in.screen_available) {
        // Grow down from where the user put it, as far as the screen allows.
        const Rect& available = *in.screen_available;
        const int room = available.y + available.height - 8 - in.y;
        return std::max(in.base_height, std::min(in.configured_max, room));
    }
    if (!in.anchor_available || !in.anchor) {
        return in.configured_max;
    }
    return max_height_for_anchor(*in.anchor, *in.anchor_available, in.configured_max);
}

std::optional<Point> reposition_near_anchor(const RepositionInput& in) {
    if (in.preview) {
        return std::nullopt;
    }
    if (in.embedded) {
        return Point{std::max(0, in.parent.width - in.overlay.width - kEmbeddedMarginRight),
                     std::max(0, in.parent.height - in.overlay.height - kEmbeddedMarginBottom)};
    }
    if (in.placed) {
        // Where the user put it stays put; growing only nudges it back on screen.
        if (in.screen_available) {
            return clamp_rect_to_available(in.current.x, in.current.y, in.overlay.width,
                                           in.overlay.height, *in.screen_available);
        }
        return std::nullopt;
    }
    if (in.cornered && in.anchor_available) {
        return Point{in.anchor_available->x + kParkMargin, in.anchor_available->y + kParkMargin};
    }
    if (!in.anchor) {
        return std::nullopt;
    }
    if (!in.anchor_available) {
        return Point{in.anchor->x + 10, in.anchor->y + 10};
    }
    return preferred_overlay_position(*in.anchor, in.overlay.width, in.overlay.height,
                                      *in.anchor_available);
}

} // namespace fv
