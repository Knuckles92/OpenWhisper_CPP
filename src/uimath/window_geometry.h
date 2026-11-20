#pragma once

#include <string_view>

#include "base/json.h"
#include "uimath/overlay_position.h"

namespace fv {

// A window size in logical pixels.
struct Size {
    int width = 0;
    int height = 0;
    friend bool operator==(const Size&, const Size&) = default;
};

// The main window's sizes (OpenWhisper's AppConfig MAIN_WINDOW_*), in logical pixels.
namespace window_size {
inline constexpr int kMinWidth = 650;
inline constexpr int kMinHeight = 660;
inline constexpr int kDefaultWidth = 680;
inline constexpr int kDefaultHeight = 680;
inline constexpr int kMaxWidth = 1280;
inline constexpr int kHistorySidebarWidth = 380;
inline constexpr int kHistoryEdgeTabWidth = 24;
} // namespace window_size

// What window_geometry's "format" says when the saved width leaves the side panel out.
inline constexpr std::string_view kCollapsedContentFormat = "collapsed_content_v1";

// MainWindow._clamp_geometry: the candidate rectangle kept wholly on one screen's available
// area. The width is held between min.width and the smaller of max.width and the screen's
// width, the height between min.height and the screen's height (max.height is not applied, as
// the window's maximum height never was), and the origin is moved onto the screen. A minimum
// larger than the screen wins and pins the window to the screen's top-left.
Rect clamp_geometry(Rect candidate, Rect available, Size min, Size max);

// MainWindow._restored_width: the saved width of `geometry` (an object with at least "width")
// clamped to [min_width, max_width]. A width saved by a version that counted the open side
// panel in it (its "format" is not `format`, and the width is at least the default width plus
// the panel minus the edge tab) loses the panel's width first. A width at or below the default
// width, and a migrated one, become the default width. Throws std::invalid_argument when
// "width" is missing or not an integer (true and false count as 1 and 0, as Python's bool is an
// int), or does not fit an int: Python's QRect refused those, and the window was centred.
int restored_width(const Json& geometry, int min_width, int max_width, std::string_view format);

// A saved coordinate or size as Python's QRect took it: an integer, or a bool as 0 or 1.
// Throws std::invalid_argument naming `key` for anything else or a value beyond an int.
int geometry_int(const Json& geometry, std::string_view key);

} // namespace fv
