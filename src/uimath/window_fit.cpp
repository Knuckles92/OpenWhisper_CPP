#include "uimath/window_fit.h"

#include <algorithm>

namespace fv {

// main_window.py:1372-1374.
int fit_height(const FitInput& input) {
    return std::min(input.chrome_height + input.content_height, input.available_height);
}

// main_window.py:1378.
int fit_floor(int fit_height, int min_height) {
    return std::min(fit_height, min_height);
}

FitPlan plan_fit(const FitInput& input) {
    FitPlan plan;
    plan.fit = fit_height(input);
    plan.floor = fit_floor(plan.fit, input.min_height);
    plan.target = plan.fit;
    if (input.elastic) {
        plan.target = std::max(plan.fit, input.user_sized ? input.user_height : 0);
    }
    plan.interim_min_height = std::min(plan.floor, input.current_height);
    return plan;
}

// main_window.py:1309-1331.
PageWidth page_width(const PageWidthInput& input) {
    if (input.last_need && input.need == *input.last_need) {
        return PageWidth{input.window_width, input.own_width};
    }
    const int own = input.own_width ? *input.own_width + input.panel_width : input.window_width;
    // Everything beside the page's content (side panel included) stays as wide.
    const int wanted = input.window_width - input.content_width + input.need;
    if (wanted <= own) {
        return PageWidth{own, std::nullopt};
    }
    return PageWidth{wanted, input.own_width
                                 ? input.own_width
                                 : std::optional<int>(input.window_width - input.panel_width)};
}

// main_window.py:1251-1259.
CollapseResize transcript_collapse(bool collapsed, int delta, int current_height,
                                   int transcript_height, int min_height) {
    CollapseResize resize;
    if (collapsed) {
        resize.transcript_height = current_height;
        resize.target_height = current_height - delta;
        resize.min_height = std::min(min_height, resize.target_height);
    } else {
        resize.target_height = std::max(current_height + delta, transcript_height);
        resize.transcript_height = resize.target_height;
    }
    return resize;
}

// main_window.py:1844-1847.
PanelStep side_panel_step(int base_width, int panel_width, int max_width) {
    const int limit = max_width + panel_width;
    return PanelStep{limit, std::min(limit, base_width + panel_width)};
}

// main_window.py:2302-2310.
int saved_window_width(int width, int min_width, bool panel_expanded, std::optional<int> own_width,
                       int panel_width) {
    if (panel_expanded) {
        width = std::max(min_width, width - panel_width);
    }
    if (own_width) {
        width = *own_width;
    }
    return width;
}

// main_window.py:1618-1622; QRect's right() is x + width - 1.
Point compact_position(Point saved, Size window, const Rect& available) {
    const int right = available.x + available.width - 1;
    const int bottom = available.y + available.height - 1;
    return Point{std::min(std::max(saved.x, available.x), right - window.width + 1),
                 std::min(std::max(saved.y, available.y), bottom - window.height + 1)};
}

} // namespace fv
