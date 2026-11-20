#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "uimath/look_sims.h"
#include "uimath/overlay_position.h"
#include "uimath/window_geometry.h"

// The recording overlay's layout decisions (ui_qt/overlays/waveform_overlay.py): where the badge
// row, the preview header or footer, the words, the divider and the label row go, how tall the
// overlay grows for its words, and how a look draws small in the header. They are plain
// functions of what the widget measured (text and badge widths, line heights, the screen), so
// they run and test without a font. Sizes are logical pixels, the overlay's own, y down.

namespace fv {

inline constexpr int kOverlayWidth = 300;
// The compact overlay: the look and its label row.
inline constexpr int kOverlayBaseHeight = 80;
// The tallest the overlay grows for preview words.
inline constexpr int kOverlayStreamingMaxHeight = 400;

inline constexpr int kBadgeHeight = 18;
inline constexpr int kBadgeMargin = 9;
// The badge row, which the compact look sits under whenever something is in it.
inline constexpr int kTopRow = kBadgeMargin + kBadgeHeight;
// How tall a look draws in the compact overlay, above its label row.
inline constexpr double kLookHeight = 56.0;
// The preview header's row, and the small copy of the look in it.
inline constexpr int kHeaderHeight = 36;
inline constexpr double kHeaderLookHeight = 24.0;
// The narrowest room the header gives its look; less shows only the clock.
inline constexpr double kHeaderLookMinWidth = 40.0;
// The preview words' side margins.
inline constexpr int kTextMargin = 12;
// The header's line of its own for the live label and small look, under a badge row with no
// room for them (a walkthrough's Snap and Done).
inline constexpr int kLiveRow = 28;
// The label row: its height, its distance from the bottom of the band, and its side inset.
inline constexpr int kStatusHeight = 20;
inline constexpr int kStatusFromBottom = 25;
inline constexpr int kStatusInset = 12;

enum class PreviewLayout { Header, Footer };

// QRectF::toRect as Qt 6 rounds: the corner rounds half away from zero (qRound), and each side
// rounds after taking half of what rounding moved the corner, so (10.3, 0, 5.3, 1) is
// (10, 0, 5, 1), not the (10, 0, 6, 1) that rounding both edges gives.
Rect to_rect(const RectD& rect);

// What the top row's contents depend on.
struct TopRowInput {
    int width = kOverlayWidth;
    // The destination, Hands-free or Command Mode badge is up, and its width.
    bool badge_shown = false;
    double badge_width = 0.0;
    // A listening state (recording, streaming, command listening).
    bool listening = false;
    // The language chip is up (a language and two or more choices), and its width.
    bool chip_visible = false;
    double chip_width = 0.0;
    // Snap and Done sit along the top and take clicks, and the left edge of the leftmost.
    bool controls_clickable = false;
    double controls_left = 0.0;
};

// Whether a badge, the language chip or Snap and Done sit along the top.
bool top_row_in_use(const TopRowInput& in);

// Where the compact look's band starts: under the badge row whenever something is in it.
int band_top(const TopRowInput& in);

// The badge row's free (left, right): between the badge and the chip or Snap and Done.
struct Span {
    double left = 0.0;
    double right = 0.0;
};
Span top_row_span(const TopRowInput& in);

// Whether the header's live label and small look fit in the badge row. live_label_width is the
// label's width without words (clock at its widest, 0:00) when a live look is drawing and the
// dot or clock is on, else nullopt.
bool live_shares_top_row(const TopRowInput& in, std::optional<double> live_label_width);

// The preview header: the badge row, and the live label's own line if it needs one.
int header_height(const TopRowInput& in, std::optional<double> live_label_width);

// The room above and below the preview words.
struct Margins {
    int above = 0;
    int below = 0;
};
Margins preview_text_margins(PreviewLayout layout, int base_height, bool top_row_in_use,
                             int header_height);

// Where the words go: (12, above, width - 24, max(20, height - above - below)).
Rect preview_text_rect(int width, int height, Margins margins);

// The y of the 1 px divider between the words and the look; the line is drawn at y + 0.5 from
// x 12 to width - 12. Header: under the header. Footer: over the full band.
int divider_y(PreviewLayout layout, int height, int base_height, int header_height);

// The footer's band starts at height - base_height; the header layout's look is the header's.
inline int footer_band_top(int height, int base_height) {
    return height - base_height;
}

// The label row: under the look, wherever the preview layout put it. With words, the footer's
// is the bottom 20 px and the header's is the header's look room widened by 12 px each side
// (a caption takes that room); without them it sits under the compact band.
Rect status_rect(bool has_words, PreviewLayout layout, int width, int height, int band_top,
                 int base_height, const RectD& header_room);

// What _draw_preview_header decides.
struct HeaderRowInput {
    int width = kOverlayWidth;
    // live_shares_top_row, and the badge row's free span when it does.
    bool shares_top_row = true;
    Span top_row_span;
    // The label parts: the live dot and the clock.
    bool dot = true;
    bool clock = true;
    // A live look is drawing (Classic has no label).
    bool live = true;
    // The label's width without words, with the clock as set and with it dropped.
    double label_width = 0.0;
    double label_width_without_clock = 0.0;
    // A caption is up: it takes the look's room.
    bool caption = false;
};

struct HeaderRow {
    // Draw "● 0:04" in label_rect; label_clock says whether the clock is part of it (it is
    // dropped when Snap and Done crowd the row).
    bool draw_label = false;
    bool label_clock = false;
    RectD label_rect;
    // The room left for the small look (the overlay's _header_room); the look is drawn into it
    // only when draw_look.
    RectD room;
    bool draw_look = false;
};
HeaderRow header_row(const HeaderRowInput& in);

// How a look draws small into area (Visual.small): a transform the painter applies after
// translating to area's top-left, and the width the look draws at in its own units.
struct SmallLook {
    // The clip rectangle, in overlay pixels.
    RectD clip;
    // A point (x, y) of the look lands at (origin_x + x * scale_x, origin_y + y * scale_y).
    double origin_x = 0.0;
    double origin_y = 0.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    // The look's own width: nullopt for fit, which draws the overlay's whole drawing at the
    // overlay's width, else the room's width in the look's units (squash: unscaled, shrink:
    // divided by the scale). It is the look's input and never state the look keeps.
    std::optional<double> band_width;
};
// small is kSmallSquash, kSmallShrink or kSmallFit (anything else shrinks); Classic, which has
// no look class and is only particles, passes kSmallFit. overlay_width is the overlay's own
// width.
SmallLook small_look(std::string_view small, const RectD& area, double overlay_width);

// The embedded overlay's wrapped preview lines, newest last: of line_heights (one per line, top
// to bottom) the newest that fit text_rect's height, drawn bottom-aligned.
struct VisibleLines {
    // The first line shown.
    std::size_t first = 0;
    // The y of the first shown line, and the total height shown.
    double y = 0.0;
    double height = 0.0;
};
VisibleLines visible_preview_lines(const std::vector<double>& line_heights, const Rect& text_rect);

// Growth for the preview words (_apply_height).
struct HeightInput {
    bool has_words = false;
    int base_height = kOverlayBaseHeight;
    // band_top() of the overlay.
    int band_top = 0;
    // effective_max_height().
    int max_height = kOverlayStreamingMaxHeight;
    Margins margins;
    // The height of the words wrapped to text_flow_width(), as the widget measured them.
    int text_height = 0;
};
// The width the widget wraps the words to when measuring them.
inline int text_flow_width(int overlay_width) {
    return overlay_width - 2 * kTextMargin;
}
// The overlay's height: no words, the look under its badge row (base + band_top); words, the
// text and its margins, between the base height and max_height.
int target_height(const HeightInput& in);

// The tallest the overlay may grow (_effective_streaming_max_height).
struct MaxHeightInput {
    int base_height = kOverlayBaseHeight;
    int configured_max = kOverlayStreamingMaxHeight;
    // Embedded in a window: a third of its height at most.
    bool embedded = false;
    int parent_height = 0;
    // The user dragged it (or a snap parked it): it grows down from where it is, as far as its
    // screen's available geometry allows.
    bool placed = false;
    std::optional<Rect> screen_available;
    int y = 0;
    // Else it grows from the pointer: the screen the anchor is on, and the anchor.
    std::optional<Rect> anchor_available;
    std::optional<Point> anchor;
};
int effective_max_height(const MaxHeightInput& in);

// Where the overlay goes after its height changed (_reposition_near_anchor); nullopt leaves it.
struct RepositionInput {
    bool preview = false;
    bool embedded = false;
    Size parent;
    Size overlay{kOverlayWidth, kOverlayBaseHeight};
    bool placed = false;
    Point current;
    std::optional<Rect> screen_available;
    // A walkthrough keeps it in the screen's top-left corner until dragged.
    bool cornered = false;
    std::optional<Rect> anchor_available;
    std::optional<Point> anchor;
};
inline constexpr int kParkMargin = 16;
inline constexpr int kEmbeddedMarginRight = 16;
inline constexpr int kEmbeddedMarginBottom = 64;
std::optional<Point> reposition_near_anchor(const RepositionInput& in);

} // namespace fv
