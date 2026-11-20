#pragma once

#include "ui_overlay/visual.h"

namespace fv {

// Ribbon: a glowing band tracing the last couple of seconds; the particles leave its crest.
// Finish: the ribbon rolls itself up into a spinning loop. The band and its colours are
// VoiceRibbon::shape(). Affinity: GUI thread.
class RibbonLook final : public Visual {
public:
    using Visual::Visual;

    std::string_view small() const override { return kSmallSquash; }
    std::function<double(double)> emitter() const override;
    void draw(QPainter& painter, double grow, double width) override;
    void drawWork(QPainter& painter, double morph, double clock, double width) override;

private:
    // The particles' launch line: the overlay's base height less kBaselineFromBottom.
    double baseline() const;
};

} // namespace fv
