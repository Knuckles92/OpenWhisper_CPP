#pragma once

#include <optional>

#include "uimath/overlay_position.h"
#include "uimath/window_geometry.h"

// The main window's decisions about its own size (ui_qt/main_window.py): fitting the height to
// the open page, widening for a page and handing the width back, the transcript-collapse
// resize, the side panel's lockstep width, the saved width and Compact Mode's position. All
// sizes are logical pixels.

namespace fv {

// The rest of OpenWhisper's AppConfig MAIN_WINDOW_* sizes (config.py:322-327).
namespace window_size {
// The height an open transcript first gets.
inline constexpr int kTranscriptionExpandHeight = 840;
inline constexpr int kCompactWidth = 420;
inline constexpr int kCompactHeight = 250;
} // namespace window_size

// What _sync_page_size measures before it fits the window to a page.
struct FitInput {
    int chrome_height = 0;    // the window's height less the page stack's
    int content_height = 0;   // the page content's height at the width the window is going to
    int current_height = 0;   // the window's height now
    int available_height = 0; // the screen's available height (_max_usable_height)
    int min_height = window_size::kMinHeight;
    // An open transcript or a storyboard: it fills whatever height the user gives it.
    bool elastic = false;
    // The user gave this elastic page a height (user_height); Python's preferred height was
    // None until then.
    bool user_sized = false;
    int user_height = 0;
};

// The window height that shows all of the page without scrolling, capped at the available
// height.
int fit_height(const FitInput&);
// How short the window may go on the page: its fit, capped so a long page can still be shrunk
// to scroll (min(fit, 660)).
int fit_floor(int fit_height, int min_height);

// main_window.py:1372-1395 as numbers.
struct FitPlan {
    int fit = 0;
    int floor = 0;
    // fit, or the user's height for an elastic page when that is taller.
    int target = 0;
    // The minimum height to set before resizing: the floor lowered to the current height, so a
    // shrink is not stopped short; a rise waits for the window to be there.
    int interim_min_height = 0;
};
FitPlan plan_fit(const FitInput&);

// What _page_width decides from.
struct PageWidthInput {
    int need = 0;                 // the content width the page asks for (_page_content_width)
    std::optional<int> last_need; // the need it last widened for
    int window_width = 0;
    int panel_width = 0;          // the side panel's current width
    int content_width = 0;        // the page's scroll-area contents width now
    std::optional<int> own_width; // the window's width without the panel from before a widening
};
struct PageWidth {
    int width = 0;
    std::optional<int> own_width;
};
// The window width for a page: unchanged while its need is; else widened while the page needs
// more than the user's own width, and the own width handed back once a page fits in it.
PageWidth page_width(const PageWidthInput&);

// _on_transcription_collapsed for a page that is not the one fitted to its content.
struct CollapseResize {
    int target_height = 0;
    // The open transcript's height to remember for the next expand.
    int transcript_height = 0;
    // A lowered minimum height to set first, when collapsing below it.
    std::optional<int> min_height;
};
CollapseResize transcript_collapse(bool collapsed, int delta, int current_height,
                                   int transcript_height, int min_height);

// _on_sidebar_width_animated: the width cap is the page's, and the side panel adds its own on
// top, so opening it never squeezes the page while the screen has the room.
struct PanelStep {
    int limit = 0;  // the window's maximum width for this frame
    int wanted = 0; // the window width before the screen clamps it
};
PanelStep side_panel_step(int base_width, int panel_width, int max_width = window_size::kMaxWidth);

// _save_geometry's width: without the open side panel, and without a page's widening.
int saved_window_width(int width, int min_width, bool panel_expanded, std::optional<int> own_width,
                       int panel_width = window_size::kHistorySidebarWidth);

// _restore_compact_geometry: the saved top-left moved onto the screen. A window wider or taller
// than the screen keeps its right or bottom edge on it, as Python's min(max(...)) did.
Point compact_position(Point saved, Size window, const Rect& available);

} // namespace fv
