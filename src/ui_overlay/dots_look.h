#pragma once

#include "ui_overlay/visual.h"

namespace fv {

// Dots: five dots that ripple with the voice, newest on the right. Finish: they drop into a row
// and bounce in turn like a typing indicator (uimath's dots_frame and dots_finish).
// Affinity: GUI thread.
class DotsLook final : public Visual {
public:
    using Visual::Visual;

    bool particles() const override { return false; }
    void draw(QPainter& painter, double grow, double width) override;
    void drawWork(QPainter& painter, double morph, double clock, double width) override;
};

} // namespace fv
