#pragma once

#include "ui_overlay/visual.h"

namespace fv {

// Bars, the default look: a scrolling meter of rounded bars, mirrored about the middle, one
// history sample each, newest at the right. Finish: the bars fly into a ring of spokes with a
// highlight running round. Affinity: GUI thread.
class BarsLook final : public Visual {
public:
    // Pixels between bar starts, a bar's width, and the tallest half-bar.
    static constexpr double kPitch = 5.0;
    static constexpr double kBar = 3.0;
    static constexpr double kHalf = 18.0;
    static constexpr int kSpokes = 28;

    using Visual::Visual;

    bool particles() const override { return false; }
    std::string_view small() const override { return kSmallSquash; }
    void draw(QPainter& painter, double grow, double width) override;
    void drawWork(QPainter& painter, double morph, double clock, double width) override;
};

} // namespace fv
